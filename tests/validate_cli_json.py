import json
import subprocess
import sys


program, fixture = sys.argv[1:3]
command = [
    program,
    "--input",
    fixture,
    "--strict",
    "--symbol",
    "AAPL",
    "--top",
    "10",
    "--format",
    "json",
]
first = subprocess.run(command, check=True, capture_output=True, text=True)
second = subprocess.run(command, check=True, capture_output=True, text=True)
assert first.stdout == second.stdout
document = json.loads(first.stdout)
assert document["schema"] == "lobforge_replay_v1"
assert document["records_seen"] == 28
assert document["records_failed"] == 0
assert document["state_digest_fnv1a64"] == "eaa0ddd8309c94c0"
assert document["bids"] == [{"price": 1234400, "shares": 80, "orders": 1}]
assert document["asks"] == []
