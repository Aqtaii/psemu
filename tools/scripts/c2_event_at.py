# psemu: data.js icinde verilen offset civarindaki C2 event blogunu, KOSULLARI
# gorunur olacak sekilde genis pencereyle dokum eder.
# Kullanim: python c2_event_at.py <offset> [geri] [ileri]
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

off = int(sys.argv[1]) if len(sys.argv) > 1 else 888479
back = int(sys.argv[2]) if len(sys.argv) > 2 else 1200
fwd = int(sys.argv[3]) if len(sys.argv) > 3 else 300

s = d[max(0, off - back):off + fwd]
print(s.replace("],[", "],\n["))
