import asyncio
import json
import time

from websockets.server import serve


async def echo(websocket):
    async for message in websocket:
        try:
            event = json.loads(message)
        except Exception as e:
            response = {
                "event_type": "PARSE_ERROR",
                "message": f"Parsing of '{message}' failed.",
                "error": str(e),
            }
            await websocket.send(json.dumps(response))
            continue

        if "PROCESS_IMAGE_REQUEST" == event["event_type"]:
            # actually remove background
            time.sleep(2)
            filename = event["filename"]
            response = {
                "event_type": "PROCESS_IMAGE_RESPONSE",
                "processing_status": "DONE",
                "message": f"finished processing of {filename}",
            }
            await websocket.send(f"{response}")
        else:
            response = {
                "event_type": "UNKNOWN_EVENT_ERROR",
                "message": f"Unknown event in: '{message}'.",
            }
            await websocket.send(f"{response}")


async def main():
    async with serve(echo, "localhost", 8080):
        await asyncio.Future()  # run forever


asyncio.run(main())
