const PORT = 4777;

const server = Bun.serve<{ ip: string }>({
    port: PORT,
    tls: {
        cert: Bun.file("cert.pem"),
        key: Bun.file("key.pem"),
    },
    fetch(req, server) {
        const success = server.upgrade(req, {
            data: {
                ip: server.requestIP(req)?.address || "Unknown",
            },
        });
        if (success) return undefined;

        return new Response("Upgrade failed", { status: 500 });
    },
    websocket: {
        open(ws) {
            console.log(`New device connected: ${ws.data.ip}`);
            ws.subscribe("broadcast");
        },
        message(ws, message) {
            // When we receive a command (from Browser), broadcast it to everyone (ESP32)
            // ws.publish sends to all subscribers EXCEPT the sender
            ws.publish("broadcast", message);
        },
        close(ws) {
            console.log('Device disconnected');
            ws.unsubscribe("broadcast");
        },
    },
});

console.log(`🚀 Motor Control Server running on port ${server.port} (WSS)`);
console.log(`Make sure your ESP32 and Browser connect using wss://!`);
console.log(`Ensure 'cert.pem' and 'key.pem' are in the socket directory.`);
