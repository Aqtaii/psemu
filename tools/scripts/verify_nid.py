import hashlib, base64
print(base64.b64encode(hashlib.sha1(b'scePthreadMutexLock').digest()[:8]).decode())
