#!/usr/bin/env python3
"""The evaluation gateway of the AI column feature.

The gateway has three jobs:

``serve``
    An OpenAI compatible inference service.  Without an upstream the answers
    of the service are deterministic, so the result of an AI column does not
    depend on the moment the test runs.  With ``--upstream`` the service is a
    transparent proxy: the request is forwarded to the upstream and the answer
    of the upstream is returned unchanged.  In both modes the service can
    inject a delay, a failure or a lost answer, and it records every request
    so that a test can assert what the model has been asked and how much the
    evaluation has cost.

``stats``
    Prints the recorded requests: the number of the requests, the number of
    the tokens and the estimated cost of the evaluation.

``verify``
    Runs every acceptance check of the AI column feature and prints a summary.
    The check scripts are the ones of the ``checks`` directory next to the
    gateway, they use the same semantics as the official mysqltest suite.

The gateway is a tool of its own, it is not part of the transaction boundary
of the database.  Its unit test uses a fake upstream, it needs no credentials
of a real model.

Usage:
    python tools/ai_gateway/gateway.py serve --port 19130
    python tools/ai_gateway/gateway.py serve --port 19130 --upstream http://127.0.0.1:8000
    python tools/ai_gateway/gateway.py stats --port 19130
    python tools/ai_gateway/gateway.py verify --port 2881
"""
import argparse
import json
import os
import random
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STATE_ROOT = os.path.join(HERE, 'state')

JSON_TYPE_HINT = 'valid JSON value'
INT_TYPE_HINT = 'base-10 integer'

# The name of the file that holds the recorded requests.
EVENTS_FILE = 'events.jsonl'
# The name of the file that holds the statistics of the evaluation.
STATS_FILE = 'stats.json'


def state_dir(port):
    return os.path.join(STATE_ROOT, str(port))


def events_path(port):
    return os.path.join(state_dir(port), EVENTS_FILE)


def stats_path(port):
    return os.path.join(state_dir(port), STATS_FILE)


def read_events(port):
    events = []
    path = events_path(port)
    if os.path.exists(path):
        with open(path, 'r', encoding='utf-8') as stream:
            for line in stream:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    return events


def build_answer(user_text, system_text):
    """The deterministic answer of the model."""
    if INT_TYPE_HINT in system_text:
        return '7'
    if JSON_TYPE_HINT in system_text:
        return json.dumps({'category': 'mock', 'input': user_text}, ensure_ascii=False)
    return 'mock:' + user_text


def build_vector(user_text):
    if 'vec-zero' in user_text:
        return [0.0, 0.0, 0.0, 0.0]
    if 'vec-ten' in user_text:
        return [10.0, 0.0, 0.0, 0.0]
    if 'vec-twenty' in user_text:
        return [20.0, 0.0, 0.0, 0.0]
    return [1.0, 0.25, 0.5, 0.75]


def count_tokens(text):
    """The number of the tokens of a text, the estimation is enough for the
    statistics of the evaluation."""
    if not text:
        return 0
    return max(1, len(text) // 4)


def count_request_tokens(body):
    """The number of the tokens of the prompt of a request."""
    total = 0
    for message in body.get('messages', []):
        total += count_tokens(str(message.get('content', '')))
    values = body.get('input', '')
    if isinstance(values, list):
        for value in values:
            total += count_tokens(str(value))
    elif values:
        total += count_tokens(str(values))
    return total


def count_answer_tokens(body):
    """The number of the tokens of the answer of a request."""
    total = 0
    for choice in body.get('choices', []):
        message = choice.get('message', {})
        total += count_tokens(str(message.get('content', '')))
    for item in body.get('data', []):
        total += count_tokens(str(item.get('embedding', '')))
    return total


def build_stats(events, price_per_1k):
    """The statistics of the recorded requests."""
    prompt_tokens = 0
    answer_tokens = 0
    failures = 0
    dropped = 0
    for event in events:
        prompt_tokens += event.get('prompt_tokens', 0)
        answer_tokens += event.get('answer_tokens', 0)
        if event.get('status', 0) >= 500:
            failures += 1
        if event.get('dropped'):
            dropped += 1
    total_tokens = prompt_tokens + answer_tokens
    cost = total_tokens / 1000.0 * price_per_1k
    return {
        'requests': len(events),
        'prompt_tokens': prompt_tokens,
        'answer_tokens': answer_tokens,
        'total_tokens': total_tokens,
        'failures': failures,
        'dropped': dropped,
        'price_per_1k': price_per_1k,
        'cost': round(cost, 6),
    }


def write_stats(port, stats):
    with open(stats_path(port), 'w', encoding='utf-8') as stream:
        json.dump(stats, stream, ensure_ascii=False, indent=1)


def serve(port, upstream='', delay_ms=0, fail_rate=0.0, drop_response=False,
          price_per_1k=0.0, seed=0):
    os.makedirs(state_dir(port), exist_ok=True)
    with open(events_path(port), 'w', encoding='utf-8'):
        pass
    lock = threading.Lock()
    random.seed(seed)
    upstream_url = upstream.rstrip('/') if upstream else ''

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, fmt, *args):
            return

        def do_POST(self):
            size = int(self.headers.get('Content-Length', '0'))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode('utf-8'))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            system_text = ''
            user_text = ''
            for message in body.get('messages', []):
                if message.get('role') == 'system':
                    system_text = str(message.get('content', ''))
                elif message.get('role') == 'user':
                    user_text = str(message.get('content', ''))
            if parsed.path.endswith('/embeddings'):
                values = body.get('input', '')
                if isinstance(values, list):
                    user_text = '|'.join(str(value) for value in values)
                else:
                    user_text = str(values)
            event = {
                'path': parsed.path,
                'upstream': upstream_url,
                'system': system_text,
                'user': user_text,
                'authorization': self.headers.get('Authorization', ''),
                'prompt_tokens': count_request_tokens(body),
                'answer_tokens': 0,
                'status': 200,
                'dropped': False,
            }
            # a failure of the evaluation is injected before the request is
            # forwarded, the client has to see the very same failure as it
            # would see when the model itself fails
            if fail_rate > 0 and random.random() < fail_rate:
                event['status'] = 500
                self.record(lock, event)
                self.reply(500, {'error': {'message': 'the gateway injected a failure'}})
                return
            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)
            if upstream_url:
                status, response = self.forward(upstream_url + self.path, raw)
                event['status'] = status
                event['answer_tokens'] = count_answer_tokens(response)
                if drop_response:
                    # the model has answered, the answer is lost on the way
                    # back, the client has to time out
                    self.lose(lock, event)
                    return
                self.record(lock, event)
                self.reply(status, response)
                return
            if 'slow' in user_text:
                time.sleep(1.2)
            else:
                time.sleep(0.05)
            if parsed.path.endswith('/embeddings'):
                values = body.get('input', [])
                if not isinstance(values, list):
                    values = [values]
                response = {
                    'data': [{'index': index, 'embedding': build_vector(str(value))}
                             for index, value in enumerate(values)]
                }
            else:
                response = {
                    'choices': [{'message': {'role': 'assistant',
                                             'content': build_answer(user_text, system_text)}}]
                }
            event['answer_tokens'] = count_answer_tokens(response)
            if drop_response:
                self.lose(lock, event)
                return
            self.record(lock, event)
            self.reply(200, response)

        def forward(self, url, raw):
            """Forwards the request to the upstream, the answer of the
            upstream is returned unchanged."""
            status = 502
            response = {'error': {'message': 'the upstream is not reachable'}}
            request = urllib.request.Request(url, data=raw, method='POST')
            authorization = self.headers.get('Authorization', '')
            if authorization:
                request.add_header('Authorization', authorization)
            content_type = self.headers.get('Content-Type', '')
            if content_type:
                request.add_header('Content-Type', content_type)
            try:
                with urllib.request.urlopen(request, timeout=60) as upstream:
                    payload = upstream.read()
                    status = upstream.status
                try:
                    response = json.loads(payload.decode('utf-8'))
                except Exception:
                    response = {'raw': payload.decode('utf-8', 'replace')}
            except urllib.error.HTTPError as error:
                status = error.code
                payload = error.read()
                try:
                    response = json.loads(payload.decode('utf-8'))
                except Exception:
                    response = {'raw': payload.decode('utf-8', 'replace')}
            except Exception:
                # the upstream is not reachable, the gateway answers like a
                # broken model would
                status = 502
            return status, response

        def lose(self, lock, event):
            """Loses the answer of the model, the connection is closed
            without a reply, the client has to time out."""
            event['dropped'] = True
            self.record(lock, event)
            self.close_connection = True
            try:
                self.wfile.close()
            except Exception:
                pass

        def record(self, lock, event):
            with lock:
                with open(events_path(port), 'a', encoding='utf-8') as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + '\n')
                stats = build_stats(read_events(port), price_per_1k)
                write_stats(port, stats)

        def reply(self, status, response):
            payload = json.dumps(response, ensure_ascii=False).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    print('[gateway] listening on 127.0.0.1:{}'.format(port), flush=True)
    if upstream_url:
        print('[gateway] the upstream is {}'.format(upstream_url), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def stats(port):
    events = read_events(port)
    price_per_1k = 0.0
    if os.path.exists(stats_path(port)):
        with open(stats_path(port), 'r', encoding='utf-8') as stream:
            try:
                price_per_1k = float(json.load(stream).get('price_per_1k', 0.0))
            except Exception:
                price_per_1k = 0.0
    summary = build_stats(events, price_per_1k)
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    return 0


# The acceptance checks of the AI column feature, the name is the name of the
# script.  They live next to the gateway, the scripts directory of the
# workspace is accepted as well.
CHECK_DIRS = [
    os.path.join(HERE, 'checks'),
    os.path.join(ROOT, 'scripts'),
]
CHECKS = [
    ('ddl', 'ai_column_ddl_check.py'),
    ('dml', 'ai_dml_link_check.py'),
    ('worker', 'ai_worker_check.py'),
    ('vector', 'ai_vector_check.py'),
    ('retry', 'ai_retry_check.py'),
]


def verify(port):
    results = []
    for name, script in CHECKS:
        path = ''
        for directory in CHECK_DIRS:
            candidate = os.path.join(directory, script)
            if os.path.exists(candidate):
                path = candidate
                break
        if not path:
            results.append((name, 'missing', ''))
            continue
        print('[gateway] running the check {}'.format(name), flush=True)
        proc = subprocess.run([sys.executable, path, '--port', str(port)],
                              capture_output=True, text=True)
        summary = ''
        for line in (proc.stdout or '').splitlines():
            if line.startswith('[RESULT]'):
                summary = line.strip()
        ok = proc.returncode == 0
        results.append((name, 'PASS' if ok else 'FAIL', summary))
        if not ok:
            print(proc.stdout or '')
            print(proc.stderr or '')
    print('')
    print('[gateway] the summary of the evaluation')
    failed = 0
    for name, state, summary in results:
        print('  {:<8} {:<6} {}'.format(name, state, summary))
        if state != 'PASS':
            failed += 1
    print('[RESULT] {}/{} checks passed'.format(len(results) - failed, len(results)))
    return 0 if failed == 0 else 1


def main():
    ap = argparse.ArgumentParser(description='the evaluation gateway of the AI column feature')
    sub = ap.add_subparsers(dest='command')
    serve_parser = sub.add_parser('serve', help='run the inference service')
    serve_parser.add_argument('--port', type=int, default=19130)
    serve_parser.add_argument('--upstream', default='',
                               help='the upstream of the transparent proxy')
    serve_parser.add_argument('--delay-ms', type=int, default=0,
                               help='the delay that is added to every request')
    serve_parser.add_argument('--fail-rate', type=float, default=0.0,
                               help='the share of the requests that fail')
    serve_parser.add_argument('--drop-response', action='store_true',
                               help='the answer of the upstream is lost')
    serve_parser.add_argument('--price-per-1k', type=float, default=0.0,
                               help='the price of a thousand tokens')
    serve_parser.add_argument('--seed', type=int, default=0)
    stats_parser = sub.add_parser('stats', help='print the statistics of the evaluation')
    stats_parser.add_argument('--port', type=int, default=19130)
    verify_parser = sub.add_parser('verify', help='run every acceptance check')
    verify_parser.add_argument('--port', type=int, default=2881)
    args = ap.parse_args()
    if args.command == 'serve':
        return serve(args.port, args.upstream, args.delay_ms, args.fail_rate,
                     args.drop_response, args.price_per_1k, args.seed)
    if args.command == 'stats':
        return stats(args.port)
    if args.command == 'verify':
        return verify(args.port)
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
