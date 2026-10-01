#!/usr/bin/env python3
"""
Ponte WebSocket <-> UDP para testar o app web com o dispositivo falso.

O firmware real atende o app web em ws://IP/ws (hal_net). No computador,
esta ponte faz o mesmo papel na frente do fake_device: cada mensagem
binária recebida pelo WebSocket é repassada como datagrama UDP ao
fake_device, e cada resposta UDP volta como mensagem binária.

Uso:
    make fake_device && ./fake_device &
    pip install websockets
    python3 ws_bridge.py [porta_ws=80] [porta_udp=54322]
"""
import asyncio
import socket
import sys

import websockets

WS_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 80
UDP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 54322


async def handle(ws):
    path = getattr(getattr(ws, "request", None), "path", "/ws")
    if path != "/ws":
        await ws.close(code=1008, reason="use /ws")
        return

    loop = asyncio.get_running_loop()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.setblocking(False)

    async def udp_to_ws():
        while True:
            data = await loop.sock_recv(sock, 4096)
            await ws.send(data)

    relay = asyncio.create_task(udp_to_ws())
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                await loop.sock_sendto(sock, bytes(message), ("127.0.0.1", UDP_PORT))
    finally:
        relay.cancel()
        sock.close()


async def main():
    async with websockets.serve(handle, "0.0.0.0", WS_PORT):
        print(f"ponte ws://0.0.0.0:{WS_PORT}/ws -> udp 127.0.0.1:{UDP_PORT}", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
