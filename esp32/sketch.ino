#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

// --- CONFIGURATION ---
const char *ssid = "MatchaLatte";
const char *password = "04072006";

// !!! IMPORTANT: CHANGE THIS TO YOUR LAPTOP'S IP ADDRESS !!!
const char *websocket_server_host = "yourserver.net";
const int websocket_server_port = 4777;

// Motor Pins (L298N)
const int PIN_ENA = 25;
const int PIN_IN1 = 13;
const int PIN_IN2 = 14;

// Motor PWM Settings
const int PWM_CHANNEL = 0;
const int PWM_FREQ = 1000;
const int PWM_RESOLUTION = 8;
// Minimum PWM to make the motor actually spin (kills the mosquito noise)
const int MIN_PWM = 50;
// Maximum PWM (Since 5V source - 2V Drop = 3V, we can use full 255 safely)
const int MAX_PWM = 255;

// --- SAFETY SETTINGS ---
// Watchdog timeout (ms) - stop motor if no command received within this time
const unsigned long WATCHDOG_TIMEOUT_MS = 3000;
// Max acceleration per update (prevents sudden jerks, protects gears/motor)
const int MAX_ACCELERATION = 25; // Max PWM change per command
// Minimum brake time before direction change (ms)
const unsigned long DIRECTION_CHANGE_BRAKE_MS = 100;

// --- STATE ---
WebSocketsClient webSocket;
unsigned long lastCommandTime = 0;
int currentPwm = 0;
int targetPwm = 0;
int pendingTargetPwm = 0;     // Stores target speed during direction change
bool currentDirection = true; // true = forward
bool targetDirection = true;
bool isChangingDirection = false;
unsigned long directionChangeStartTime = 0;
bool motorEnabled = true;

// Forward declarations
void moveMotor(bool forward, int speedPercent);
void stopMotor();
void emergencyStop();
void applyMotorState();

// Function to handle incoming socket messages
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.println("[WSc] Disconnected!");
    // Safety: Stop motor on disconnect
    emergencyStop();
    break;

  case WStype_CONNECTED:
    Serial.println("[WSc] Connected to Server!");
    lastCommandTime = millis();
    break;

  case WStype_TEXT: {
    // Validate payload length to prevent buffer issues
    if (length == 0 || length > 256) {
      Serial.println("[WSc] Invalid payload length");
      return;
    }

    Serial.printf("[WSc] Message: %s\n", payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
      Serial.printf("[WSc] JSON Parse Error: %s\n", error.c_str());
      return;
    }

    // Handle different message types from server
    const char *msgType = doc["type"];

    // Only process 'state' messages (ignore 'users' count messages)
    if (msgType && strcmp(msgType, "state") == 0) {
      // Validate that required fields exist
      if (!doc.containsKey("speed") || !doc.containsKey("forward")) {
        Serial.println("[WSc] Missing required fields");
        return;
      }

      int speedPercent = doc["speed"];
      bool forward = doc["forward"];

      // Update watchdog
      lastCommandTime = millis();
      motorEnabled = true;

      moveMotor(forward, speedPercent);
    }
    // Also handle legacy format (no type field)
    else if (!msgType && doc.containsKey("speed")) {
      int speedPercent = doc["speed"];
      bool forward = doc["forward"] | true; // Default to forward

      lastCommandTime = millis();
      motorEnabled = true;

      moveMotor(forward, speedPercent);
    }
  } break;

  case WStype_ERROR:
    Serial.println("[WSc] Error occurred!");
    break;

  case WStype_PING:
  case WStype_PONG:
    // Heartbeat - update watchdog
    lastCommandTime = millis();
    break;

  default:
    break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100); // Give serial time to initialize

  Serial.println("\n=== ESP32 Motor Controller ===");
  Serial.println("Initializing...");

  // Initialize motor pins to safe state FIRST
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_ENA, OUTPUT);

  // Ensure motor is stopped on startup
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_ENA, LOW);

  // Setup PWM
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PIN_ENA, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);

  Serial.println("Motor pins initialized (safe state)");

  // Connect to WiFi with timeout
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int wifiAttempts = 0;
  const int maxWifiAttempts = 40; // 20 seconds timeout

  while (WiFi.status() != WL_CONNECTED && wifiAttempts < maxWifiAttempts) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection FAILED!");
    Serial.println("Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.println("\nWiFi connected!");
  Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Signal strength (RSSI): %d dBm\n", WiFi.RSSI());

  // Initialize WebSocket
  webSocket.beginSSL(websocket_server_host, websocket_server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000,
                            2); // Ping every 15s, timeout 3s, 2 retries

  Serial.printf("WebSocket connecting to: wss://%s:%d\n", websocket_server_host,
                websocket_server_port);
  Serial.println("=== Initialization Complete ===\n");

  lastCommandTime = millis();
}

void loop() {
  webSocket.loop();

  // --- WATCHDOG TIMER ---
  // If no valid command received within timeout, stop motor for safety
  if (motorEnabled && (millis() - lastCommandTime > WATCHDOG_TIMEOUT_MS)) {
    Serial.println("[SAFETY] Watchdog timeout - stopping motor!");
    emergencyStop();
    motorEnabled = false;
  }

  // --- DIRECTION CHANGE HANDLING ---
  // When changing direction, we need to brake first to protect the motor
  if (isChangingDirection) {
    if (millis() - directionChangeStartTime >= DIRECTION_CHANGE_BRAKE_MS) {
      // Brake period complete, now apply new direction
      currentDirection = targetDirection;
      isChangingDirection = false;
      targetPwm = pendingTargetPwm; // Restore target
      Serial.printf("[Motor] Direction changed to: %s, restoring PWM to %d\n",
                    currentDirection ? "FORWARD" : "REVERSE", targetPwm);
    } else {
      // Still braking - keep motor stopped
      return;
    }
  }

  // --- SMOOTH ACCELERATION ---
  // Gradually approach target PWM to prevent sudden jerks
  if (currentPwm != targetPwm) {
    int diff = targetPwm - currentPwm;

    if (abs(diff) <= MAX_ACCELERATION) {
      currentPwm = targetPwm;
    } else if (diff > 0) {
      currentPwm += MAX_ACCELERATION;
    } else {
      currentPwm -= MAX_ACCELERATION;
    }

    applyMotorState();
  }

  // --- WIFI RECONNECTION ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SAFETY] WiFi lost - stopping motor!");
    emergencyStop();
    motorEnabled = false;

    // Attempt reconnection
    Serial.println("Attempting WiFi reconnection...");
    WiFi.reconnect();
    delay(5000);
  }

  // Small delay to prevent tight loop
  delay(10);
}

// Set motor target (with safety checks)
void moveMotor(bool forward, int speedPercent) {
  // 1. INPUT CLAMPING (Safety)
  speedPercent = constrain(speedPercent, 0, 100);

  // 2. CALCULATE TARGET PWM
  if (speedPercent == 0) {
    targetPwm = 0;
  } else {
    // Map 1-100% to working PWM range
    targetPwm = map(speedPercent, 1, 100, MIN_PWM, MAX_PWM);
  }

  // 3. HANDLE DIRECTION CHANGE
  // Only initiate direction change if motor is running and direction differs
  if (forward != currentDirection && currentPwm > 0) {
    Serial.println("[Motor] Direction change requested - braking first");
    pendingTargetPwm = targetPwm; // Save intended target
    targetPwm = 0;                // Force stop for braking
    targetDirection = forward;
    isChangingDirection = true;
    directionChangeStartTime = millis();

    // Immediately stop for braking
    ledcWrite(PWM_CHANNEL, 0);
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, LOW);
  } else {
    targetDirection = forward;
    if (currentPwm == 0) {
      // If motor is stopped, we can change direction immediately
      currentDirection = forward;
    }
  }

  Serial.printf("[Motor] Target: %d%% (%s) -> PWM: %d\n", speedPercent,
                targetDirection ? "FWD" : "REV", targetPwm);
}

// Apply current motor state to hardware
void applyMotorState() {
  // Set direction pins
  if (currentDirection) {
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, LOW);
  } else {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, HIGH);
  }

  // Apply PWM
  ledcWrite(PWM_CHANNEL, currentPwm);
}

// Graceful stop
void stopMotor() {
  targetPwm = 0;
  currentPwm = 0;
  ledcWrite(PWM_CHANNEL, 0);
  Serial.println("[Motor] Stopped");
}

// Emergency stop - immediate halt
void emergencyStop() {
  targetPwm = 0;
  currentPwm = 0;
  isChangingDirection = false;

  // Immediate stop - both pins LOW = brake
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  ledcWrite(PWM_CHANNEL, 0);

  Serial.println("[EMERGENCY] Motor stopped immediately!");
}