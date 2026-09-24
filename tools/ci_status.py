"""查看 splatscan 的 GitHub Actions 状态，必要时打印失败步骤的日志尾部。

用法：
    python3 tools/ci_status.py            # 最近 5 次运行 + 最新一次的分步状态
    python3 tools/ci_status.py --logs     # 额外打印失败步骤的日志
"""

import io
import json
import sys
import urllib.request
import zipfile

OWNER, REPO = 'happy-everyday-everyweek', 'splatscan'
API = 'https://api.github.com/repos/%s/%s' % (OWNER, REPO)


class StripAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None:
            new.headers.pop('Authorization', None)
        return new


OPENER = urllib.request.build_opener(StripAuthOnRedirect)


def token():
    for line in open('/root/.git-credentials'):
        line = line.strip()
        if 'github.com' in line:
            return line.split('://', 1)[1].split(':', 1)[1].split('@', 1)[0]
    raise SystemExit('no github token')


TOK = token()


def get_bytes(url):
    request = urllib.request.Request(
        url,
        headers={'Authorization': 'token ' + TOK, 'Accept': 'application/vnd.github+json'},
    )
    with OPENER.open(request, timeout=90) as response:
        return response.read()


def get_json(url):
    return json.loads(get_bytes(url).decode())


def main():
    want_logs = '--logs' in sys.argv
    runs = get_json(API + '/actions/runs?per_page=5')['workflow_runs']
    if not runs:
        print('no runs yet')
        return
    for run in runs:
        print('%s  %s  %s  %s  %s' % (
            run['id'], run['status'], run['conclusion'], run['head_sha'][:7], run['created_at']))

    latest = runs[0]
    print('\n--- run %s ---' % latest['id'])
    jobs = get_json(API + '/actions/runs/%s/jobs' % latest['id'])['jobs']
    for job in jobs:
        print('job %s: %s %s' % (job['name'], job['status'], job['conclusion']))
        for step in job['steps']:
            mark = 'x' if step['conclusion'] == 'failure' else ' '
            print('  [%s] %2d %s -> %s' % (mark, step['number'], step['name'], step['conclusion']))

    if not want_logs:
        return

    for job in jobs:
        if job['conclusion'] != 'failure':
            continue
        print('\n--- 失败步骤日志（job %s） ---' % job['name'])
        data = get_bytes(API + '/actions/jobs/%s/logs' % job['id'])
        text = data.decode('utf-8-sig', 'replace')
        interesting = [line for line in text.splitlines()
                       if ('error:' in line or 'FAILED' in line or 'CMake Error' in line
                           or 'What went wrong' in line or 'e: ' in line)]
        for line in interesting[:80]:
            print(line[-240:])


if __name__ == '__main__':
    main()