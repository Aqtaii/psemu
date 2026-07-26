import base64, hashlib, re
SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
def nid(n):
    h = hashlib.sha1(n.encode()+SALT).digest()[:8][::-1]
    return base64.b64encode(h).decode().replace(chr(47), chr(45))[:11]
have = open("include/nids.h", encoding="utf-8", errors="replace").read()
for name in ["__cxa_pure_virtual","__gxx_personality_v0","malloc","free","memcpy","memset",
             "_Stdout","_Stderr","__stack_chk_guard","_ZSt7nothrow"]:
    k = nid(name)
    print(f'    {{"{k}#s#s", "{name}"}},   // nids.h icinde: {"VAR" if k in have else "YOK"}')
