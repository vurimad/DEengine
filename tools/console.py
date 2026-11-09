"""Runs console commands in a running DEengine (dev builds), through its agents' socket (tools/agent_server.h).

The same text as the console window (`) takes, so a person and an agent change the engine the same way:

    python tools/console.py "stat gpu; r.taa 0"
    python tools/console.py help
    python tools/console.py --complete r.
    python tools/console.py --port 37018 "camera.goto 10 2 40"

The engine says which port it listens on when it starts ("commands on 127.0.0.1:N"): 37017, or the next free one
when another engine has it. Prints what the commands say; exits 1 if any failed, 2 if no engine answered.
"""

import argparse
import json
import socket
import sys

DEFAULT_PORT = 37017


def request(port, method, params, timeout_s):
    message = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    with socket.create_connection(("127.0.0.1", port), timeout=timeout_s) as connection:
        connection.sendall((json.dumps(message) + "\n").encode("utf-8"))
        answer = b""
        while not answer.endswith(b"\n"):
            chunk = connection.recv(65536)
            if not chunk:
                break
            answer += chunk
    return json.loads(answer.decode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description="Run console commands in a running DEengine.")
    parser.add_argument("line", nargs="*", help="the commands, as typed in the console (';' between several)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="the engine's port (default %(default)s)")
    parser.add_argument("--complete", metavar="PREFIX", help="list the commands and variables starting with PREFIX")
    parser.add_argument("--timeout", type=float, default=10.0, help="seconds to wait for the answer")
    args = parser.parse_args()

    try:
        if args.complete is not None:
            response = request(args.port, "console.complete", {"prefix": args.complete}, args.timeout)
        else:
            if not args.line:
                parser.error("no command given")
            response = request(args.port, "console.exec", {"line": " ".join(args.line)}, args.timeout)
    except OSError as error:
        print(f"no engine answered on 127.0.0.1:{args.port}: {error}", file=sys.stderr)
        return 2

    if "error" in response:
        print(response["error"].get("message", "the engine refused the request"), file=sys.stderr)
        return 1
    result = response["result"]
    if args.complete is not None:
        for entry in result:
            print(f"{entry['name']:<20} {entry['usage']:<20} {entry['help']}")
        return 0
    for line in result["output"]:
        print(line)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
