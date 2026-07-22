import sys
with open('nid_db.txt', 'r', encoding='utf-8') as f:
    lines = f.readlines()

out = "#pragma once\n#include <string>\n#include <unordered_map>\n\ninline std::unordered_map<std::string, std::string> g_nid_to_name = {\n"
for l in lines:
    parts = l.split()
    if len(parts) == 2:
        nid, name = parts[0], parts[1]
        out += f'    {{"{nid}#T#T", "{name}"}},\n'
        out += f'    {{"{nid}#S#N", "{name}"}},\n'
        out += f'    {{"{nid}#T#N", "{name}"}},\n'
        out += f'    {{"{nid}#S#T", "{name}"}},\n'

out += "};\n"

with open('include/nids.h', 'w', encoding='utf-8') as f:
    f.write(out)
