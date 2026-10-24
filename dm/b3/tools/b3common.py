#!/usr/bin/env python3
# B3 工具的公共部分：统一的报告信封、退出码、输出目录与脱敏规则。dm/b3/tools 下所有产出报告的脚本都用它，
# 这样每份报告都能用同一条命令校验：python3 dm/b3/tools/b3common.py validate <报告.json>。
# 只用 Python 标准库，支持 Python 3.9 及以上（macOS 自带的 python3）。
# Shared parts of the B3 tools: one report envelope, exit codes, output directory and redaction rules. Every report-producing
# script under dm/b3/tools uses it, so any report can be checked with one command: python3 dm/b3/tools/b3common.py validate <report.json>.
# Standard library only, Python 3.9 or later (the python3 shipped with macOS).
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))

# 报告状态与退出码一一对应 | Report status and exit code correspond one to one
EXIT = {'PASS': 0, 'FAIL': 1, 'BLOCKED': 2, 'ERROR': 3, 'SKIP': 4}
ENVELOPE_KEYS = ('schema', 'tool', 'tool_version', 'git_sha', 'git_dirty', 'created_utc', 'status', 'summary',
                 'inputs', 'data', 'unknowns', 'notes')
SCHEMA_RE = re.compile(r'^b3\.[a-z][a-z0-9_]*/[1-9][0-9]*$')
# 报告里不许出现的个人绝对路径 | Personal absolute paths that must not appear in a report
ABS_PATH_RE = re.compile(r'(^|[\s"\'=:(])/(Users|home|private/var/folders|var/folders|sessions)/')
# 疑似密钥：32 字节以上的十六进制串旁边出现 key / secret / token 字样 | Likely secrets: key / secret / token next to a long hex string
SECRET_RE = re.compile(r'(?i)(key|secret|token|password)\W{0,3}[0-9a-f]{32,}')


def redact(text):
    """把家目录换成 ~ | Replace the home directory with ~"""
    home = os.path.expanduser('~')
    if home and home != '/' and isinstance(text, str):
        text = text.replace(home, '~')
    return text


def sha256_file(path, chunk=1 << 20):
    """文件的 SHA256 十六进制串 | SHA256 hex digest of a file"""
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def git_info(root=REPO_ROOT):
    """(HEAD 的 SHA, 工作区是否有改动)；不在 git 里时为 (None, None) | (HEAD SHA, whether the work tree is dirty); (None, None) outside git"""
    try:
        sha = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        st = subprocess.run(['git', '-C', root, 'status', '--porcelain', '--untracked-files=no'],
                            capture_output=True, text=True, check=True).stdout
        return sha, bool(st.strip())
    except (OSError, subprocess.CalledProcessError):
        return None, None


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).strftime('%Y-%m-%dT%H:%M:%SZ')


def make_report(kind, tool, status, summary, data, inputs=(), unknowns=(), notes=(), tool_version='1'):
    """组一份报告；kind 是 schema 名去掉前缀，如 'doctor' → 'b3.doctor/1' | Build a report; kind is the schema name without prefix"""
    sha, dirty = git_info()
    rep = {
        'schema': kind if kind.startswith('b3.') else f'b3.{kind}/1',
        'tool': tool,
        'tool_version': str(tool_version),
        'git_sha': sha,
        'git_dirty': dirty,
        'created_utc': utc_now(),
        'status': status,
        'summary': summary,
        'inputs': list(inputs),
        'data': data,
        'unknowns': list(unknowns),
        'notes': list(notes),
    }
    return _redact_tree(rep)


def input_entry(name, path=None):
    """inputs 里的一项：只记名字、大小和 SHA256，不记路径 | One inputs entry: name, size and SHA256 only, no path"""
    e = {'name': name}
    if path is not None and os.path.isfile(path):
        e['size'] = os.path.getsize(path)
        e['sha256'] = sha256_file(path)
    return e


def _redact_tree(v):
    if isinstance(v, dict):
        return {k: _redact_tree(x) for k, x in v.items()}
    if isinstance(v, list):
        return [_redact_tree(x) for x in v]
    return redact(v)


def _walk_strings(v, where='$'):
    if isinstance(v, dict):
        for k, x in v.items():
            yield from _walk_strings(x, f'{where}.{k}')
    elif isinstance(v, list):
        for i, x in enumerate(v):
            yield from _walk_strings(x, f'{where}[{i}]')
    elif isinstance(v, str):
        yield where, v


def validate(rep):
    """返回错误列表；空列表表示合格 | Return a list of errors; empty means valid"""
    errs = []
    if not isinstance(rep, dict):
        return ['report is not a JSON object']
    for k in ENVELOPE_KEYS:
        if k not in rep:
            errs.append(f'missing key: {k}')
    extra = sorted(set(rep) - set(ENVELOPE_KEYS))
    if extra:
        errs.append(f'unexpected top-level keys (put them under data): {extra}')
    if 'schema' in rep and not (isinstance(rep['schema'], str) and SCHEMA_RE.match(rep['schema'])):
        errs.append(f'bad schema: {rep.get("schema")!r} (want b3.<kind>/<n>)')
    if 'status' in rep and rep['status'] not in EXIT:
        errs.append(f'bad status: {rep.get("status")!r} (want one of {sorted(EXIT)})')
    if 'summary' in rep and not (isinstance(rep['summary'], str) and rep['summary'].strip() and '\n' not in rep['summary']):
        errs.append('summary must be one non-empty line')
    for k in ('inputs', 'unknowns', 'notes'):
        if k in rep and not isinstance(rep[k], list):
            errs.append(f'{k} must be a list')
    if 'data' in rep and not isinstance(rep['data'], dict):
        errs.append('data must be an object')
    if rep.get('git_sha') is not None and not re.match(r'^[0-9a-f]{40}$', str(rep.get('git_sha'))):
        errs.append('git_sha must be a 40-hex SHA or null')
    for where, s in _walk_strings(rep):
        if ABS_PATH_RE.search(s):
            errs.append(f'personal absolute path at {where}: use ~ or a relative path')
        if SECRET_RE.search(s):
            errs.append(f'possible secret at {where}')
    return errs


def default_out_dir():
    """缺省输出目录：$DM_ROOT/reports，没设 DM_ROOT 时 ~/b3-work/runs/adhoc/reports | Default output directory"""
    root = os.environ.get('DM_ROOT') or os.path.join(os.path.expanduser('~'), 'b3-work', 'runs', 'adhoc')
    return os.path.join(root, 'reports')


def _inside_repo(path):
    p = os.path.realpath(path)
    r = os.path.realpath(REPO_ROOT)
    return p == r or p.startswith(r + os.sep)


def write_report(rep, out_dir=None, name=None):
    """校验后写出报告；不合格或目录在仓库里则抛 ValueError | Validate then write; raise ValueError if invalid or inside the repo"""
    errs = validate(rep)
    if errs:
        raise ValueError('invalid report: ' + '; '.join(errs))
    out_dir = out_dir or default_out_dir()
    if _inside_repo(out_dir):
        raise ValueError(f'refusing to write a report inside the repository: {redact(out_dir)}')
    os.makedirs(out_dir, exist_ok=True)
    if name is None:
        name = rep['schema'].split('/')[0][3:] + '.json'
    path = os.path.join(out_dir, name)
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(rep, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write('\n')
    return path


def exit_code(rep):
    return EXIT.get(rep.get('status'), EXIT['ERROR'])


def finish(rep, out_dir=None, name=None, quiet=False):
    """写出报告、打印一行结论并按状态退出 | Write the report, print one result line and exit by status"""
    path = write_report(rep, out_dir, name)
    if not quiet:
        print(f'{rep["schema"]} {rep["status"]}: {rep["summary"]}')
        print(f'report: {redact(path)}')
    sys.exit(exit_code(rep))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) < 2 or argv[0] != 'validate':
        print('usage: b3common.py validate <report.json>...', file=sys.stderr)
        return EXIT['ERROR']
    bad = 0
    for p in argv[1:]:
        try:
            with open(p, encoding='utf-8') as f:
                rep = json.load(f)
        except (OSError, ValueError) as e:
            print(f'FAIL {redact(p)}: cannot read JSON: {e}')
            bad += 1
            continue
        errs = validate(rep)
        if errs:
            bad += 1
            print(f'FAIL {redact(p)}')
            for e in errs:
                print(f'  - {e}')
        else:
            print(f'OK   {redact(p)} ({rep["schema"]} {rep["status"]})')
    return EXIT['FAIL'] if bad else EXIT['PASS']


if __name__ == '__main__':
    sys.exit(main())
