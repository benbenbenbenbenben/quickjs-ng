import argparse
import json
import pathlib
import select
import socket
import subprocess
import tempfile
import textwrap
import time
import unittest
from collections import deque


def find_free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class DAPClient:
    def __init__(self, qjs_path, script_source):
        self.qjs_path = str(pathlib.Path(qjs_path).resolve())
        self.script_source = textwrap.dedent(script_source).lstrip()
        self.port = find_free_port()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.script_path = pathlib.Path(self.tmpdir.name, "script.js")
        self.script_path.write_text(self.script_source)
        self.proc = None
        self.sock = None
        self.seq = 1
        self.queue = deque()

    def connect(self):
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.2)
                self.sock.settimeout(5)
                self.queue.clear()
                return
            except OSError:
                if self.proc is not None and self.proc.poll() is not None:
                    raise RuntimeError(f"qjs exited early with status {self.proc.returncode}")
                time.sleep(0.05)
        raise RuntimeError("timed out waiting for DAP server")

    def start(self):
        self.proc = subprocess.Popen(
            [self.qjs_path, f"--dap=tcp:{self.port}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.connect()

    def close_socket(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        self.queue.clear()

    def reconnect(self):
        self.close_socket()
        self.connect()

    def close(self):
        self.close_socket()
        if self.proc is not None:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
            self.proc = None
        self.tmpdir.cleanup()

    def send_request(self, command, arguments=None):
        seq = self.seq
        request = {
            "seq": seq,
            "type": "request",
            "command": command,
        }
        if arguments is not None:
            request["arguments"] = arguments
        self.seq += 1
        payload = json.dumps(request).encode()
        header = f"Content-Length: {len(payload)}\r\n\r\n".encode()
        self.sock.sendall(header + payload)
        return seq

    def _recv_message(self, timeout=5):
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if not ready:
            raise TimeoutError("timed out waiting for DAP message")
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            chunk = self.sock.recv(1)
            if not chunk:
                return None
            header += chunk
        content_length = 0
        for line in header.decode().split("\r\n"):
            if line.startswith("Content-Length: "):
                content_length = int(line.split(": ", 1)[1])
                break
        body = b""
        while len(body) < content_length:
            chunk = self.sock.recv(content_length - len(body))
            if not chunk:
                return None
            body += chunk
        return json.loads(body.decode())

    def next_message(self, timeout=5):
        if self.queue:
            return self.queue.popleft()
        message = self._recv_message(timeout=timeout)
        if message is None:
            raise EOFError("DAP connection closed")
        return message

    def expect_response(self, seq, command, success=True, timeout=5):
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError(f"timed out waiting for response to {command}")
            msg = self.next_message(timeout=remaining)
            if msg.get("type") == "response" and msg.get("request_seq") == seq:
                self._assert_equal(msg.get("command"), command, msg)
                self._assert_equal(msg.get("success"), success, msg)
                return msg
            self.queue.append(msg)

    def wait_for_event(self, event, timeout=5, predicate=None):
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError(f"timed out waiting for event {event}")
            msg = self.next_message(timeout=remaining)
            if msg.get("type") == "event" and msg.get("event") == event:
                if predicate is None or predicate(msg):
                    return msg
            self.queue.append(msg)

    def initialize(self):
        seq = self.send_request("initialize", {"adapterID": "python-test"})
        response = self.expect_response(seq, "initialize")
        self.wait_for_event("initialized")
        return response

    def launch(self, *, stop_on_entry=False, args=None, module=None):
        launch_args = {"program": str(self.script_path), "stopOnEntry": stop_on_entry}
        if args is not None:
            launch_args["args"] = args
        if module is not None:
            launch_args["module"] = module
        seq = self.send_request("launch", launch_args)
        return self.expect_response(seq, "launch")

    def attach(self):
        seq = self.send_request("attach", {})
        return self.expect_response(seq, "attach")

    def configuration_done(self):
        seq = self.send_request("configurationDone")
        return self.expect_response(seq, "configurationDone")

    def disconnect(self, terminate_debuggee=False):
        arguments = {"terminateDebuggee": terminate_debuggee} if terminate_debuggee else {}
        seq = self.send_request("disconnect", arguments)
        return self.expect_response(seq, "disconnect")

    @staticmethod
    def _assert_equal(actual, expected, msg):
        if actual != expected:
            raise AssertionError(f"expected {expected!r}, got {actual!r} in {msg!r}")


class DAPIntegrationTests(unittest.TestCase):
    qjs_path = None

    def make_client(self, script):
        client = DAPClient(self.qjs_path, script)
        client.start()
        self.addCleanup(client.close)
        return client

    def get_top_frame(self, client):
        seq = client.send_request("stackTrace", {"threadId": 1})
        response = client.expect_response(seq, "stackTrace")
        return response["body"]["stackFrames"][0]

    def get_scopes(self, client, frame_id):
        seq = client.send_request("scopes", {"frameId": frame_id})
        return client.expect_response(seq, "scopes")["body"]["scopes"]

    def get_scope_refs(self, client, frame_id):
        return {scope["name"]: scope["variablesReference"] for scope in self.get_scopes(client, frame_id)}

    def get_variables(self, client, variables_reference):
        seq = client.send_request("variables", {"variablesReference": variables_reference})
        response = client.expect_response(seq, "variables")
        return {entry["name"]: entry["value"] for entry in response["body"]["variables"]}

    def test_launch_stop_on_entry_and_output(self):
        client = self.make_client(
            """
            print("hello from dap");
            """
        )
        init = client.initialize()
        self.assertTrue(init["body"]["supportsTerminateRequest"])
        client.launch(stop_on_entry=True)
        client.configuration_done()

        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "entry")

        seq = client.send_request("threads")
        threads = client.expect_response(seq, "threads")
        self.assertEqual(threads["body"]["threads"][0]["name"], "main")

        frame = self.get_top_frame(client)
        self.assertEqual(pathlib.Path(frame["source"]["path"]).resolve(), client.script_path.resolve())

        seq = client.send_request("continue", {"threadId": 1})
        client.expect_response(seq, "continue")
        client.wait_for_event("continued")
        output = client.wait_for_event("output", predicate=lambda msg: "hello from dap" in msg["body"]["output"])
        self.assertIn("hello from dap", output["body"]["output"])
        exited = client.wait_for_event("exited")
        self.assertEqual(exited["body"]["exitCode"], 0)
        client.wait_for_event("terminated")

    def test_attach_requires_running_target(self):
        client = self.make_client(
            """
            print("unused");
            """
        )
        client.initialize()
        seq = client.send_request("attach", {})
        response = client.expect_response(seq, "attach", success=False)
        self.assertIn("running target", response["message"])
        client.disconnect()

    def test_breakpoint_variables_set_variable_and_globals(self):
        client = self.make_client(
            """
            function test() {
                let x = 1;
                let y = 2;
                print(x, y, globalThis.flag);
            }
            globalThis.flag = 0;
            test();
            """
        )
        client.initialize()
        client.launch()
        seq = client.send_request(
            "setBreakpoints",
            {
                "source": {"path": str(client.script_path)},
                "breakpoints": [{"line": 4}],
            },
        )
        response = client.expect_response(seq, "setBreakpoints")
        self.assertTrue(response["body"]["breakpoints"][0]["verified"])
        client.configuration_done()

        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "breakpoint")

        frame = self.get_top_frame(client)
        scope_refs = self.get_scope_refs(client, frame["id"])
        locals_ref = scope_refs["Locals"]
        globals_ref = scope_refs["Globals"]

        locals_before = self.get_variables(client, locals_ref)
        self.assertEqual(locals_before["x"], "1")
        self.assertEqual(locals_before["y"], "2")

        seq = client.send_request("evaluate", {"expression": "1 + 1", "frameId": frame["id"]})
        evaluate = client.expect_response(seq, "evaluate")
        self.assertEqual(evaluate["body"]["result"], "2")

        seq = client.send_request("setVariable", {"variablesReference": locals_ref, "name": "x", "value": "41"})
        set_local = client.expect_response(seq, "setVariable")
        self.assertEqual(set_local["body"]["value"], "41")

        seq = client.send_request("setVariable", {"variablesReference": globals_ref, "name": "flag", "value": "7"})
        set_global = client.expect_response(seq, "setVariable")
        self.assertEqual(set_global["body"]["value"], "7")

        locals_after = self.get_variables(client, locals_ref)
        globals_after = self.get_variables(client, globals_ref)
        self.assertEqual(locals_after["x"], "41")
        self.assertEqual(globals_after["flag"], "7")

        seq = client.send_request("continue", {"threadId": 1})
        client.expect_response(seq, "continue")
        client.wait_for_event("continued")
        output = client.wait_for_event("output", predicate=lambda msg: "41 2 7" in msg["body"]["output"])
        self.assertIn("41 2 7", output["body"]["output"])
        client.wait_for_event("exited")
        client.wait_for_event("terminated")

    def test_conditional_breakpoint(self):
        client = self.make_client(
            """
            function test() {
                let x = 1;
                x += 1;
                print(x);
            }
            test();
            """
        )
        client.initialize()
        client.launch()
        seq = client.send_request(
            "setBreakpoints",
            {
                "source": {"path": str(client.script_path)},
                "breakpoints": [{"line": 4, "condition": "x === 2"}],
            },
        )
        client.expect_response(seq, "setBreakpoints")
        client.configuration_done()

        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "breakpoint")
        self.assertEqual(stopped["body"]["hitBreakpointIds"], [1])

        frame = self.get_top_frame(client)
        scope_refs = self.get_scope_refs(client, frame["id"])
        locals_after = self.get_variables(client, scope_refs["Locals"])
        self.assertEqual(locals_after["x"], "2")

        seq = client.send_request("continue", {"threadId": 1})
        client.expect_response(seq, "continue")
        client.wait_for_event("continued")
        output = client.wait_for_event("output", predicate=lambda msg: "2" in msg["body"]["output"])
        self.assertIn("2", output["body"]["output"])
        client.wait_for_event("exited")
        client.wait_for_event("terminated")

    def test_debugger_and_step_commands(self):
        client = self.make_client(
            """
            function inner(v) {
                let next = v + 1;
                return next;
            }
            function outer() {
                let x = 1;
                debugger;
                x = inner(x);
                x = x + 1;
                return x;
            }
            print(outer());
            """
        )
        client.initialize()
        client.launch()
        seq = client.send_request(
            "setBreakpoints",
            {
                "source": {"path": str(client.script_path)},
                "breakpoints": [{"line": 8}],
            },
        )
        client.expect_response(seq, "setBreakpoints")
        client.configuration_done()

        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "debugger_statement")
        self.assertEqual(self.get_top_frame(client)["name"], "outer")

        seq = client.send_request("continue", {"threadId": 1})
        client.expect_response(seq, "continue")
        client.wait_for_event("continued")
        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "breakpoint")
        self.assertEqual(self.get_top_frame(client)["name"], "outer")

        seq = client.send_request("stepIn", {"threadId": 1})
        client.expect_response(seq, "stepIn")
        client.wait_for_event("continued")
        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "step")
        self.assertEqual(pathlib.Path(self.get_top_frame(client)["source"]["path"]).resolve(), client.script_path.resolve())

        seq = client.send_request("stepOut", {"threadId": 1})
        client.expect_response(seq, "stepOut")
        client.wait_for_event("continued")
        output = client.wait_for_event("output", predicate=lambda msg: "3" in msg["body"]["output"])
        self.assertIn("3", output["body"]["output"])
        client.wait_for_event("exited")
        client.wait_for_event("terminated")

    def test_exception_info(self):
        client = self.make_client(
            """
            function boom() {
                throw new TypeError("boom");
            }
            try {
                boom();
            } catch (err) {
                print(err.name);
            }
            """
        )
        client.initialize()
        client.launch()
        seq = client.send_request("setExceptionBreakpoints", {"filters": ["all"]})
        client.expect_response(seq, "setExceptionBreakpoints")
        client.configuration_done()

        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "exception")

        seq = client.send_request("exceptionInfo", {"threadId": 1})
        info = client.expect_response(seq, "exceptionInfo")
        self.assertTrue(info["body"]["exceptionId"])
        self.assertEqual(info["body"]["breakMode"], "always")

        client.disconnect()

    def test_closure_scope_and_set_variable(self):
        client = self.make_client(
            """
            function outer() {
                let captured = 40;
                function inner() {
                    debugger;
                    print(captured);
                }
                inner();
            }
            outer();
            """
        )
        client.initialize()
        client.launch()
        client.configuration_done()

        stopped = client.wait_for_event("stopped")
        self.assertEqual(stopped["body"]["reason"], "debugger_statement")

        frame = self.get_top_frame(client)
        scope_refs = self.get_scope_refs(client, frame["id"])
        self.assertIn("Closure", scope_refs)

        closure_vars = self.get_variables(client, scope_refs["Closure"])
        self.assertEqual(closure_vars["captured"], "40")

        seq = client.send_request(
            "setVariable",
            {"variablesReference": scope_refs["Closure"], "name": "captured", "value": "41"},
        )
        updated = client.expect_response(seq, "setVariable")
        self.assertEqual(updated["body"]["value"], "41")

        seq = client.send_request("continue", {"threadId": 1})
        client.expect_response(seq, "continue")
        client.wait_for_event("continued")
        output = client.wait_for_event("output", predicate=lambda msg: "41" in msg["body"]["output"])
        self.assertIn("41", output["body"]["output"])
        client.wait_for_event("exited")
        client.wait_for_event("terminated")

    def test_pause_while_running(self):
        client = self.make_client(
            """
            let value = 0;
            while (value < 500000000) {
                value++;
            }
            print("done", value);
            """
        )
        client.initialize()
        client.launch()
        client.configuration_done()

        seq = client.send_request("pause", {"threadId": 1})
        client.expect_response(seq, "pause")
        stopped = client.wait_for_event("stopped", timeout=10)
        self.assertEqual(stopped["body"]["reason"], "pause")

        seq = client.send_request("terminate")
        client.expect_response(seq, "terminate")
        client.proc.wait(timeout=30)

    def test_disconnect_while_running_keeps_program_alive(self):
        client = self.make_client("print('placeholder');")
        done_path = client.script_path.with_name("done.txt")
        client.script_path.write_text(
            textwrap.dedent(
                f"""
                import * as std from "qjs:std";

                let value = 0;
                while (value < 5000000) {{
                    value++;
                }}
                for (let i = 0; i < 32; i++) {{
                    print("after-disconnect", i);
                }}
                let file = std.open({json.dumps(str(done_path))}, "w");
                file.puts("finished\\n");
                file.close();
                """
            ).lstrip()
        )
        client.initialize()
        client.launch(module=True)
        client.configuration_done()

        client.disconnect()
        client.close_socket()
        client.proc.wait(timeout=30)
        self.assertEqual(client.proc.returncode, 0)
        self.assertEqual(done_path.read_text(), "finished\n")

    def test_tcp_reconnect_allows_second_session(self):
        client = self.make_client(
            """
            let value = 0;
            while (value < 500000000) {
                value++;
            }
            """
        )
        client.initialize()
        client.launch()
        client.configuration_done()
        client.disconnect()
        client.reconnect()

        client.initialize()
        client.attach()
        client.configuration_done()
        seq = client.send_request("pause", {"threadId": 1})
        client.expect_response(seq, "pause")
        stopped = client.wait_for_event("stopped", timeout=20)
        self.assertEqual(stopped["body"]["reason"], "pause")

        seq = client.send_request("terminate")
        client.expect_response(seq, "terminate")
        client.proc.wait(timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qjs", help="path to qjs binary with DAP enabled")
    args = parser.parse_args()
    DAPIntegrationTests.qjs_path = args.qjs
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(DAPIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
