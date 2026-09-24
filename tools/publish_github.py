"""把当前工作区发布到 GitHub（api.github.com 直连，带重试）。

git push 走不通（github.com:443 在本机不可达），所以用 REST API：
仓库不存在先创建，再用 Git Data API 一次性写入全部文件，最后把 main 指向那个根提交。
"""

import base64
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

OWNER, REPO = 'happy-everyday-everyweek', 'splatscan'
ROOT = 'https://api.github.com/repos/%s/%s' % (OWNER, REPO)
WORK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESCRIPTION = ('SplatScan — Android 上的端上高斯泼溅 3D 扫描：'
               '实时增量重建、材料设计 3、纯本地推理，AGPL-3.0。')
COMMIT_MESSAGE = sys.argv[1] if len(sys.argv) > 1 else 'feat: SplatScan 初始版本'

socket.setdefaulttimeout(25)

TOK = None
for line in open('/root/.git-credentials'):
    line = line.strip()
    if 'github.com' in line:
        TOK = line.split('://', 1)[1].split(':', 1)[1].split('@', 1)[0]

HEAD = {
    'Authorization': 'token ' + TOK,
    'Accept': 'application/vnd.github+json',
    'User-Agent': 'splatscan-publish',
}


def api(method, url, payload=None, attempts=4):
    data = json.dumps(payload).encode() if payload is not None else None
    last = None
    for i in range(attempts):
        req = urllib.request.Request(url, data=data, headers=HEAD, method=method)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return resp.status, json.loads(resp.read().decode() or '{}')
        except urllib.error.HTTPError as e:
            try:
                body = json.loads(e.read().decode())
            except Exception:
                body = {'raw': 'http error'}
            if e.code in (502, 503, 504) and i < attempts - 1:
                time.sleep(2)
                continue
            return e.code, body
        except Exception as e:
            last = repr(e)
            time.sleep(3 * (i + 1))
    return 0, {'err': last}


def log(msg):
    print(msg, flush=True)


status, repo = api('GET', ROOT)
if status == 404:
    status, repo = api('POST', 'https://api.github.com/user/repos', {
        'name': REPO,
        'description': DESCRIPTION,
        'private': False,
        'has_issues': True,
        'has_wiki': False,
    })
    log('create repo -> %s' % status)
    time.sleep(3)
elif status == 200:
    log('repo exists')

files = subprocess.run(['git', 'ls-files'], cwd=WORK,
                       capture_output=True, text=True).stdout.split()
log('files: %d' % len(files))
if not files:
    raise SystemExit('no files to publish')

seed = files[0]
raw = open(os.path.join(WORK, seed), 'rb').read()
status, res = api('PUT', ROOT + '/contents/' + seed,
                  {'message': COMMIT_MESSAGE,
                   'content': base64.b64encode(raw).decode()})
log('seed %s -> %s' % (seed, status))
if status not in (200, 201, 409, 422):
    raise SystemExit('seed failed: %s' % res)

tree = []
for f in files:
    raw = open(os.path.join(WORK, f), 'rb').read()
    status, blob = api('POST', ROOT + '/git/blobs',
                       {'content': base64.b64encode(raw).decode(), 'encoding': 'base64'})
    if status not in (200, 201):
        log('blob FAIL %s %s %s' % (f, status, blob))
        continue
    tree.append({'path': f, 'mode': '100644', 'type': 'blob', 'sha': blob['sha']})
log('blobs ok: %d/%d' % (len(tree), len(files)))
if len(tree) != len(files):
    raise SystemExit('blob upload incomplete')

status, t = api('POST', ROOT + '/git/trees', {'tree': tree})
log('tree -> %s' % status)
if status not in (200, 201):
    raise SystemExit('tree failed: %s' % t)

status, c = api('POST', ROOT + '/git/commits',
                {'message': COMMIT_MESSAGE, 'tree': t['sha'], 'parents': []})
log('commit -> %s %s' % (status, c.get('sha')))
if status not in (200, 201):
    raise SystemExit('commit failed: %s' % c)

status, r = api('PATCH', ROOT + '/git/refs/heads/main', {'sha': c['sha'], 'force': True})
log('ref -> %s %s' % (status, r.get('object', {}).get('sha')))
status, repo = api('GET', ROOT)
log('repo pushed_at=%s size=%s' % (repo.get('pushed_at'), repo.get('size')))
