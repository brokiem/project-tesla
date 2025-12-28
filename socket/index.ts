const PORT = 4777;

const server = Bun.serve<{ ip: string }>({
    port: PORT,
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

console.log(`🚀 Motor Control Server running on port ${server.port}`);
console.log(`Make sure your ESP32 and Browser connect to your Laptop's IP!`);
