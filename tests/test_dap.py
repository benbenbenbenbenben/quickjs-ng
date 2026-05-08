import json
import subprocess
import time
import socket
import sys

def send_request(sock, command, args=None):
    request = {
        "seq": send_request.seq,
        "type": "request",
        "command": command
    }
    if args:
        request["arguments"] = args
    send_request.seq += 1

    msg = json.dumps(request)
    payload = f"Content-Length: {len(msg)}\r\n\r\n{msg}"
    print("->", payload.strip())
    sock.sendall(payload.encode('utf-8'))

send_request.seq = 1

def read_message(sock):
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        c = sock.recv(1)
        if not c:
            return None
        header += c
    
    header_str = header.decode('utf-8')
    content_length = 0
    for line in header_str.split('\r\n'):
        if line.startswith('Content-Length: '):
            content_length = int(line[16:])
            break
            
    body = b""
    while len(body) < content_length:
        chunk = sock.recv(content_length - len(body))
        if not chunk:
            return None
        body += chunk
        
    msg = json.loads(body.decode('utf-8'))
    print("<-", msg)
    return msg

def test_dap():
    script = """
function f() {
    var x = 42;
    debugger;
    console.log("Hello from inside");
    return x;
}
f();
    """
    with open("dap_test_script.js", "w") as f:
        f.write(script.lstrip())

    p = subprocess.Popen(["./zig-out/bin/qjs", "--dap=tcp:9091", "dap_test_script.js"])
    time.sleep(0.5)

    try:
        sock = socket.create_connection(("127.0.0.1", 9091), timeout=2)
        
        send_request(sock, "initialize", {"adapterID": "test"})
        msg = read_message(sock)
        msg = read_message(sock)
        
        send_request(sock, "configurationDone")
        msg = read_message(sock)
        
        # Now wait for stop (from debugger;)
        while True:
            msg = read_message(sock)
            if not msg: break
            if msg["type"] == "event" and msg["event"] == "stopped":
                break
        
        # Test scopes
        send_request(sock, "stackTrace", {"threadId": 1})
        msg = read_message(sock)
        frame_id = msg["body"]["stackFrames"][0]["id"]
        
        send_request(sock, "scopes", {"frameId": frame_id})
        msg = read_message(sock)
        locals_ref = msg["body"]["scopes"][0]["variablesReference"]
        
        send_request(sock, "variables", {"variablesReference": locals_ref})
        msg = read_message(sock)
        assert any(v["name"] == "x" for v in msg["body"]["variables"])
        
        # Step over
        send_request(sock, "next", {"threadId": 1})
        msg = read_message(sock)
        while True:
            msg = read_message(sock)
            if not msg: break
            if msg["type"] == "event" and msg["event"] == "stopped":
                break
                
        # Evaluate
        send_request(sock, "evaluate", {"expression": "1 + 1", "frameId": frame_id})
        msg = read_message(sock)
        assert msg["body"]["result"] == "2"
        
        # Continue
        send_request(sock, "continue", {"threadId": 1})
        msg = read_message(sock)
        
        got_output = False
        while True:
            msg = read_message(sock)
            if not msg: break
            if msg["type"] == "event" and msg["event"] == "output":
                if "Hello from inside" in msg["body"]["output"]:
                    got_output = True
            if msg["type"] == "event" and msg["event"] == "exited":
                break
                
        send_request(sock, "disconnect")
        
        assert got_output, "Did not receive DAP output event with Hello from inside"
        
    finally:
        sock.close()
        p.wait(timeout=2)
        print("Exit code:", p.returncode)
        subprocess.run(["rm", "dap_test_script.js"])

if __name__ == "__main__":
    test_dap()
    print("DAP integration tests passed!")
