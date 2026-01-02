#!/usr/bin/env python3
import argparse
import json
import socket
import time
from urllib.parse import urlparse


def parse_host_port(url: str) -> tuple[str, int]:
    if "://" not in url:
        url = f"tcp://{url}"
    parsed = urlparse(url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 34567
    return host, port


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Send JSON line API requests to ssh-chatter."
    )
    parser.add_argument("--url", required=True, help="Chat server URL")
    parser.add_argument("--save", required=True, help="Path to save results")
    args = parser.parse_args()

    host, port = parse_host_port(args.url)

    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(0.5)
        requests = [
            {"type": "chat", "id": 1, "username": "api-demo", "message": "hello"},
            {
                "type": "image",
                "id": 2,
                "username": "api-demo",
                "url": "https://example.com/cat.png",
                "caption": "cat",
            },
            {
                "type": "video",
                "id": 3,
                "username": "api-demo",
                "url": "https://example.com/video.mp4",
                "caption": "demo video",
            },
            {
                "type": "audio",
                "id": 4,
                "username": "api-demo",
                "url": "https://example.com/audio.mp3",
                "caption": "demo audio",
            },
            {
                "type": "files",
                "id": 5,
                "username": "api-demo",
                "url": "https://example.com/archive.zip",
                "caption": "demo file",
            },
            {
                "type": "asciiart",
                "id": 6,
                "username": "api-demo",
                "message": " /\\_/\\\n( o.o )\n > ^ <",
            },
            {
                "type": "poll",
                "id": 7,
                "username": "api-op",
                "is_operator": True,
                "question": "Favorite color?",
                "options": ["red", "blue", "green"],
            },
            {"type": "poll", "id": 8, "username": "api-demo", "action": "vote", "choice": 2},
            {
                "type": "vote",
                "id": 9,
                "username": "api-op",
                "label": "weekend",
                "question": "Plan?",
                "options": ["hike", "rest"],
                "allow_multiple": True,
            },
            {
                "type": "vote",
                "id": 10,
                "username": "api-demo",
                "label": "weekend",
                "action": "vote",
                "choice": 1,
            },
        ]

        for item in requests:
            payload = json.dumps(item, ensure_ascii=False)
            sock.sendall(payload.encode("utf-8") + b"\n")
            time.sleep(0.05)

        lines: list[str] = []
        deadline = time.time() + 3.0
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            lines.extend(
                line for line in chunk.decode("utf-8", errors="replace").splitlines()
            )

    with open(args.save, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))


if __name__ == "__main__":
    main()
