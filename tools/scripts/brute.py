import hashlib
import base64

targets = {'iPBqs+YUUFw=', 'hMAe+TWS9mQ=', 'WuMbPBKN1TU=', '9LCjpWyQ5Zc=', 'Noj9PsJrsa8=', 'fnUEjBCNRVU=', 'kALvdgEv5ME=', '9nf8joUTSaQ=', 'P8F2oavZXtY=', 'pKwslsMUmSk='}

funcs = [
    'memcpy', 'memmove', 'memset', 'memcmp', 'memchr',
    'wmemcpy', 'wmemmove', 'wmemset', 'wmemcmp', 'wmemchr',
    'strcpy', 'strncpy', 'strcat', 'strncat', 'strcmp', 'strncmp', 'strchr', 'strrchr',
    'wcscpy', 'wcsncpy', 'wcscat', 'wcsncat', 'wcscmp', 'wcsncmp', 'wcschr', 'wcsrchr',
    'malloc', 'free', 'calloc', 'realloc',
    'pthread_mutex_lock', 'pthread_mutex_unlock', 'pthread_mutex_trylock',
    'scePthreadRwlockRdlock', 'scePthreadRwlockWrlock', 'scePthreadRwlockUnlock',
    'scePthreadMutexLock', 'scePthreadMutexUnlock', 'scePthreadMutexTrylock'
]

for func in funcs:
    nid = base64.b64encode(hashlib.sha1(func.encode()).digest()[:8]).decode()
    if nid in targets:
        print(f'{nid} -> {func}')
