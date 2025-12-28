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

// Helper to broadcast current user count
function broadcastUserCount(server: Bun.Server<any>) {
    broadcastToAll(server, {
        type: 'users',
        count: server.subscriberCount("broadcast"),
    });
}

const server = Bun.serve<{ ip: string; id: string }>({
    port: PORT,
    tls: {
        cert: Bun.file("cert.pem"),
        key: Bun.file("key.pem"),
    },
    fetch(req, server) {
        const success = server.upgrade(req, {
            data: {
                ip: server.requestIP(req)?.address || "Unknown",
                id: Math.random().toString(36).substr(2, 9), // Assign ID during upgrade or in open
            },
        });
        if (success) return undefined;

        return new Response("Upgrade failed", { status: 500 });
    },
    websocket: {
        open(ws) {
            // Ensure ID exists (if not set in upgrade, set here, but it's cleaner in upgrade)
            if (!ws.data.id) ws.data.id = Math.random().toString(36).substr(2, 9);
            
            console.log(`New device connected: ${ws.data.ip} (ID: ${ws.data.id})`);
            ws.subscribe("broadcast");
            
            // Send current state to newly connected client
            ws.send(JSON.stringify({
                type: 'state',
                ...globalState,
                // Send their own ID so they know who they are (optional, but good practice)
                userId: ws.data.id 
            }));
            
            // Broadcast updated user count to all clients
            broadcastUserCount(server);
        },
        message(ws, message) {
            try {
                const data = JSON.parse(message.toString());
                
                // Handle cursor movements
                if (data.type === 'cursor') {
                    // Broadcast cursor position to ALL clients (including sender? No, client should filter)
                    // Actually, for cursors, we usually don't want to see our own laggy network cursor.
                    // But using "broadcast" topic sends to everyone. 
                    // Client will filter out its own ID.
                    ws.publish("broadcast", JSON.stringify({
                        type: 'cursor',
                        id: ws.data.id,
                        x: data.x,
                        y: data.y
                    }));
                    return;
                }

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
            console.log(`Device disconnected (ID: ${ws.data.id})`);
            ws.unsubscribe("broadcast");
            
            // Broadcast disconnect event so clients can remove cursor
            ws.publish("broadcast", JSON.stringify({
                type: 'user_disconnected',
                id: ws.data.id
            }));
            
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
