"""Call the IDA Pro MCP plugin's JSON-RPC endpoint directly, with no client-side time limit.

  python ida.py decompile_function 0x442150
  python ida.py get_xrefs_to 0x442150
  python ida.py disassemble_function 0x441f10

Arguments that look like integers are sent as strings (the plugin parses addresses itself).
Several calls can be chained: python ida.py decompile_function 0x1 , decompile_function 0x2
"""
import http.client
import json
import sys


def call(method, *params):
    conn = http.client.HTTPConnection("127.0.0.1", 13337, timeout=300)
    conn.request("POST", "/mcp", json.dumps({"jsonrpc": "2.0", "method": method,
                                             "params": list(params), "id": 1}),
                 {"Content-Type": "application/json"})
    data = json.loads(conn.getresponse().read().decode())
    if "error" in data:
        return "ERROR %s: %s" % (data["error"].get("code"), data["error"].get("message"))
    return data["result"]


def show(res):
    if isinstance(res, dict) and "result" in res and isinstance(res["result"], str):
        res = res["result"]
    if isinstance(res, str):
        # decompiler output carries /* line: N, address: X */ prefixes; keep the address only
        out = []
        for line in res.splitlines():
            if line.startswith("/* line:"):
                end = line.find("*/")
                tag = line[:end]
                addr = tag.split("address: ")[1].strip() if "address:" in tag else ""
                line = "%-8s %s" % (addr.replace("0x", ""), line[end + 2:].lstrip(" "))
            out.append(line)
        print("\n".join(out))
    else:
        print(json.dumps(res, indent=1))


def main():
    args = sys.argv[1:]
    while args:
        if "," in args:
            i = args.index(",")
            cur, args = args[:i], args[i + 1:]
        else:
            cur, args = args, []
        print("==== %s" % " ".join(cur))
        show(call(cur[0], *cur[1:]))


if __name__ == "__main__":
    main()
