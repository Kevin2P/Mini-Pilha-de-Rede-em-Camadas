import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BIN, next_ports  # noqa: E402


def spawn(args):
    return subprocess.Popen([BIN] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    base = next_ports()
    alice_port, bob_port = base, base + 1
    bob = spawn(["-a", "2", "-p", str(bob_port), "-n", "1=127.0.0.1:%d" % alice_port, "-U", "bob"])
    time.sleep(0.3)
    alice = spawn(["-a", "1", "-p", str(alice_port), "-n", "2=127.0.0.1:%d" % bob_port, "-d", "2", "-U", "alice", "-v"])
    time.sleep(0.5)
    alice.stdin.write("ola bob\n")
    alice.stdin.flush()
    time.sleep(0.4)
    bob.stdin.write("oi alice\n")
    bob.stdin.flush()
    time.sleep(0.4)
    alice.stdin.write("/quit\n")
    alice.stdin.flush()
    alice_out, alice_trace = alice.communicate(timeout=10)
    bob_out, _ = bob.communicate(timeout=10)
    print("== saida do alice ==\n" + alice_out)
    print("== saida do bob ==\n" + bob_out)
    print("== trace do alice ==")
    print(alice_trace)


main()
