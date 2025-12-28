const PORT = 4777;

// Global state
let globalState = {
    speed: 0,
    forward: true,
};

// Helper to broadcast to all clients
function broadcastToAll(server: Bun.Server<any>, message: object) {
    const payload = JSON.stringify(message);
    server.publish("broadcast", payload);
}

// Helper to broadcast the current user count
function broadcastUserCount(server: Bun.Server<any>) {
    broadcastToAll(server, {
        type: 'users',
        count: server.subscriberCount("broadcast"),
    });
}

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
            
            // Send the current state to a newly connected client
            ws.send(JSON.stringify({
                type: 'state',
                ...globalState,
            }));
            
            // Broadcast updated user count to all clients
            broadcastUserCount(server);
        },
        message(_, message) {
            try {
                const data = JSON.parse(message.toString());
                
                // Handle command messages from clients
                if (data.type === 'command' || (!data.type && data.speed !== undefined)) {
                    // Update global state
                    globalState.speed = parseInt(data.speed) || 0;
                    globalState.forward = data.forward ?? true;
                    
                    // Broadcast new state to ALL clients (including sender)
                    broadcastToAll(server, {
                        type: 'state',
                        ...globalState,
                    });
                }
            } catch (e) {
                console.error('Failed to parse message:', e);
            }
        },
        close(ws) {
            console.log('Device disconnected');
            ws.unsubscribe("broadcast");
            
            // Broadcast updated user count after disconnect
            // Use setTimeout to ensure unsubscribe is processed
            setTimeout(() => {
                broadcastUserCount(server);
            }, 0);
        },
    },
});

console.log(`🚀 Motor Control Server running on port ${server.port} (WSS)`);
console.log(`Make sure your ESP32 and Browser connect using wss://!`);
console.log(`Ensure 'cert.pem' and 'key.pem' are in the socket directory.`);
