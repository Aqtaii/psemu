lines = open('nid_db.txt', 'r', encoding='utf-8').readlines()
d = {}
for l in lines:
    parts = l.strip().split()
    if len(parts) == 2:
        d[parts[0]] = parts[1]

nids = ['WuMbPBKN1TU', '9LCjpWyQ5Zc', 'Noj9PsJrsa8', 'fnUEjBCNRVU',
        'kALvdgEv5ME', '9nf8joUTSaQ', 'P8F2oavZXtY', 'pKwslsMUmSk']
for n in nids:
    print(f"{n}: {d.get(n, 'NOT FOUND')}")
