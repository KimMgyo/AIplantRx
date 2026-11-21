"""Forward a LAN port to the llama-server on the tailnet, for testing the S3 before Funnel is on.

    python tools/llm_forward.py [listen_port] [target_host] [target_port]
"""
import asyncio
import sys

LISTEN = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
HOST = sys.argv[2] if len(sys.argv) > 2 else "100.71.14.71"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8080


async def pipe(r, w):
    try:
        while data := await r.read(65536):
            w.write(data)
            await w.drain()
    finally:
        w.close()


async def handle(cr, cw):
    try:
        sr, sw = await asyncio.open_connection(HOST, PORT)
    except OSError:
        cw.close()
        return
    await asyncio.gather(pipe(cr, sw), pipe(sr, cw), return_exceptions=True)


async def main():
    srv = await asyncio.start_server(handle, "0.0.0.0", LISTEN)
    print("forwarding :%d -> %s:%d" % (LISTEN, HOST, PORT), flush=True)
    async with srv:
        await srv.serve_forever()


asyncio.run(main())

