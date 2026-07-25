import os, re

def extract_nids(root_dir):
    nids = {}
    pattern_nid = re.compile(r'Nid\s*=\s*"([^"]+)"')
    pattern_name = re.compile(r'ExportName\s*=\s*"([^"]+)"')
    
    for dirpath, _, filenames in os.walk(root_dir):
        for fname in filenames:
            if fname.endswith('.cs'):
                filepath = os.path.join(dirpath, fname)
                try:
                    with open(filepath, 'r', encoding='utf-8') as f:
                        lines = f.readlines()
                        for i, line in enumerate(lines):
                            m1 = pattern_nid.search(line)
                            if m1:
                                nid = m1.group(1)
                                # arama (en fazla 15 satir ileri)
                                for j in range(i, min(i+15, len(lines))):
                                    m2 = pattern_name.search(lines[j])
                                    if m2:
                                        nids[nid] = m2.group(1)
                                        break
                except Exception as e:
                    pass
    return nids

if __name__ == '__main__':
    nids = extract_nids(r'D:\proje\sharpemu')
    with open('nid_db.txt', 'w', encoding='utf-8') as f:
        for nid, name in nids.items():
            f.write(f"{nid} {name}\n")
    print(f"Extracted {len(nids)} NIDs.")
