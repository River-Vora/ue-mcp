import { once } from "node:events";
import type { AddressInfo } from "node:net";
import { WebSocketServer } from "ws";
import { describe, expect, it } from "vitest";

async function withBridgeServer(
  onRequest: (request: Record<string, unknown>, socket: import("ws").WebSocket) => void,
): Promise<{
  close: () => Promise<void>;
  connectionCount: () => number;
  port: number;
}> {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  let connections = 0;

  server.on("connection", (socket) => {
    connections += 1;
    socket.on("message", (data) => {
      onRequest(JSON.parse(data.toString()) as Record<string, unknown>, socket);
    });
  });

  await once(server, "listening");
  const address = server.address() as AddressInfo;
  return {
    port: address.port,
    connectionCount: () => connections,
    close: async () => {
      for (const client of server.clients) {
        client.terminate();
      }
      await new Promise<void>((resolve, reject) => {
        server.close((err) => (err ? reject(err) : resolve()));
      });
    },
  };
}

describe("EditorBridge connection handling", () => {
  it("connects on the first bridge call when the editor bridge is reachable", async () => {
    const server = await withBridgeServer((request, socket) => {
      socket.send(JSON.stringify({ id: request.id, result: { method: request.method, params: request.params } }));
    });

    const { EditorBridge } = await import("../../src/bridge.js");
    const bridge = new EditorBridge("127.0.0.1", server.port);

    try {
      const result = await bridge.call("ping", { ok: true }, 1000);

      expect(result).toEqual({ method: "ping", params: { ok: true } });
      expect(bridge.isConnected).toBe(true);
    } finally {
      bridge.disconnect();
      await server.close();
    }
  });

  it("shares one in-flight connection for concurrent calls", async () => {
    const server = await withBridgeServer((request, socket) => {
      socket.send(JSON.stringify({ id: request.id, result: request.method }));
    });

    const { EditorBridge } = await import("../../src/bridge.js");
    const bridge = new EditorBridge("127.0.0.1", server.port);

    try {
      await expect(Promise.all([
        bridge.call("first", {}, 1000),
        bridge.call("second", {}, 1000),
      ])).resolves.toEqual(["first", "second"]);
      expect(server.connectionCount()).toBe(1);
    } finally {
      bridge.disconnect();
      await server.close();
    }
  });

  it("keeps the connection after a call times out", async () => {
    const server = await withBridgeServer((request, socket) => {
      if (request.method === "hang") return;
      socket.send(JSON.stringify({ id: request.id, result: "still here" }));
    });

    const { EditorBridge } = await import("../../src/bridge.js");
    const bridge = new EditorBridge("127.0.0.1", server.port);

    try {
      await expect(bridge.call("hang", {}, 50)).rejects.toThrow("timed out");
      // A slow editor is not a broken connection: tearing the socket down would
      // take every concurrent call with it and lose the late reply (#799).
      expect(bridge.isConnected).toBe(true);

      await expect(bridge.call("ping", {}, 1000)).resolves.toBe("still here");
      expect(server.connectionCount()).toBe(1);
    } finally {
      bridge.disconnect();
      await server.close();
    }
  });

  it("reports a timed-out call as an unknown outcome, not a failure", async () => {
    const server = await withBridgeServer(() => {});

    const { EditorBridge } = await import("../../src/bridge.js");
    const { McpError, ErrorCode } = await import("../../src/errors.js");
    const bridge = new EditorBridge("127.0.0.1", server.port);

    try {
      const error = await bridge.call("add_widget", {}, 50).catch((e: unknown) => e);

      expect(error).toBeInstanceOf(McpError);
      const mcpError = error as InstanceType<typeof McpError>;
      expect(mcpError.code).toBe(ErrorCode.BRIDGE_TIMEOUT);
      expect(mcpError.details?.outcome).toBe("unknown");
      expect(mcpError.details?.method).toBe("add_widget");
      expect(mcpError.details?.operationId).toBeTruthy();
      expect(mcpError.message).toContain("may have already applied");
    } finally {
      bridge.disconnect();
      await server.close();
    }
  });

  it("reconciles a reply that arrives after the client gave up", async () => {
    let held: { id: unknown; socket: import("ws").WebSocket } | null = null;
    const server = await withBridgeServer((request, socket) => {
      held = { id: request.id, socket };
    });

    const { EditorBridge } = await import("../../src/bridge.js");
    const bridge = new EditorBridge("127.0.0.1", server.port);

    try {
      await expect(bridge.call("add_widget", {}, 50)).rejects.toThrow("timed out");

      const pendingCall = held as unknown as { id: unknown; socket: import("ws").WebSocket };
      pendingCall.socket.send(JSON.stringify({ id: pendingCall.id, result: { created: true } }));
      await new Promise((resolve) => setTimeout(resolve, 50));

      const [abandoned] = bridge.abandonedCalls;
      expect(abandoned.method).toBe("add_widget");
      expect(abandoned.answeredAt).toBeTypeOf("number");
      expect(abandoned.result).toEqual({ created: true });
    } finally {
      bridge.disconnect();
      await server.close();
    }
  });
});
