#!/usr/bin/env python3
"""The unit test of the evaluation gateway of the AI column feature.

The test uses a fake upstream, it needs no credentials of a real model.  The
gateway is a tool of its own, it is not part of the transaction boundary of
the database, so the test only talks to the gateway.

Usage:
    python tools/ai_gateway/test_gateway.py
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY = os.path.join(HERE, 'gateway.py')


def free_port():
    """A port that nobody listens on."""
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class FakeUpstream:
    """A model service that answers with a fixed text.

    The upstream records the requests it has received, so that the test can
    assert what the gateway has forwarded.
    """

    def __init__(self, answer='the answer of the upstream'):
        self.answer = answer
        self.requests = []
        self.port = free_port()
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, fmt, *args):
                return

            def do_POST(self):
                size = int(self.headers.get('Content-Length', '0'))
                raw = self.rfile.read(size)
                upstream.requests.append({
                    'path': self.path,
                    'body': raw.decode('utf-8'),
                    'authorization': self.headers.get('Authorization', ''),
                })
                payload = json.dumps({
                    'choices': [{'message': {'role': 'assistant',
                                             'content': upstream.answer}}]
                }).encode('utf-8')
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self.server = ThreadingHTTPServer(('127.0.0.1', self.port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return 'http://127.0.0.1:{}'.format(self.port)

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class Gateway:
    """The evaluation gateway that runs in its own process."""

    def __init__(self, *options):
        self.port = free_port()
        self.process = subprocess.Popen(
            [sys.executable, GATEWAY, 'serve', '--port', str(self.port)] + list(options),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.wait_for_the_port()

    def wait_for_the_port(self):
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=1):
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError('the gateway does not listen on its port')

    def url(self, path):
        return 'http://127.0.0.1:{}{}'.format(self.port, path)

    def post(self, path, body, timeout=10):
        request = urllib.request.Request(self.url(path),
                                         data=json.dumps(body).encode('utf-8'),
                                         method='POST')
        request.add_header('Content-Type', 'application/json')
        request.add_header('Authorization', 'Bearer the-key')
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read().decode('utf-8'))

    def statistics(self):
        proc = subprocess.run([sys.executable, GATEWAY, 'stats', '--port', str(self.port)],
                              capture_output=True, text=True)
        return json.loads(proc.stdout)

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
        if self.process.stdout:
            self.process.stdout.close()


class GatewayTest(unittest.TestCase):

    def setUp(self):
        self.upstream = FakeUpstream()
        self.gateways = []

    def tearDown(self):
        for gateway in self.gateways:
            gateway.stop()
        self.upstream.stop()

    def start(self, *options):
        gateway = Gateway(*options)
        self.gateways.append(gateway)
        return gateway

    def test_the_request_is_forwarded_to_the_upstream(self):
        gateway = self.start('--upstream', self.upstream.url)
        status, body = gateway.post('/v1/chat/completions',
                                    {'messages': [{'role': 'user', 'content': 'hello'}]})
        self.assertEqual(200, status)
        self.assertEqual('the answer of the upstream',
                         body['choices'][0]['message']['content'])
        # the gateway forwarded the request and the credentials
        self.assertEqual(1, len(self.upstream.requests))
        self.assertEqual('/v1/chat/completions', self.upstream.requests[0]['path'])
        self.assertEqual('Bearer the-key', self.upstream.requests[0]['authorization'])
        self.assertIn('hello', self.upstream.requests[0]['body'])

    def test_the_delay_is_injected(self):
        gateway = self.start('--upstream', self.upstream.url, '--delay-ms', '400')
        start = time.time()
        status, _body = gateway.post('/v1/chat/completions',
                                     {'messages': [{'role': 'user', 'content': 'hello'}]})
        elapsed = time.time() - start
        self.assertEqual(200, status)
        self.assertGreaterEqual(elapsed, 0.4)

    def test_the_failure_is_injected(self):
        gateway = self.start('--upstream', self.upstream.url, '--fail-rate', '1.0')
        with self.assertRaises(urllib.error.HTTPError) as caught:
            gateway.post('/v1/chat/completions',
                         {'messages': [{'role': 'user', 'content': 'hello'}]})
        self.assertEqual(500, caught.exception.code)
        # the request did not reach the upstream
        self.assertEqual(0, len(self.upstream.requests))

    def test_the_answer_of_the_upstream_can_be_lost(self):
        gateway = self.start('--upstream', self.upstream.url, '--drop-response')
        with self.assertRaises(Exception):
            gateway.post('/v1/chat/completions',
                         {'messages': [{'role': 'user', 'content': 'hello'}]},
                         timeout=5)
        # the request reached the upstream, its answer was lost on the way
        self.assertEqual(1, len(self.upstream.requests))

    def test_the_statistics_of_the_evaluation(self):
        gateway = self.start('--upstream', self.upstream.url, '--price-per-1k', '2.0')
        gateway.post('/v1/chat/completions',
                     {'messages': [{'role': 'user', 'content': 'a long prompt of the test'}]})
        gateway.post('/v1/chat/completions',
                     {'messages': [{'role': 'user', 'content': 'another prompt'}]})
        summary = gateway.statistics()
        self.assertEqual(2, summary['requests'])
        self.assertEqual(0, summary['failures'])
        self.assertEqual(0, summary['dropped'])
        self.assertGreater(summary['prompt_tokens'], 0)
        self.assertGreater(summary['answer_tokens'], 0)
        self.assertEqual(summary['total_tokens'],
                         summary['prompt_tokens'] + summary['answer_tokens'])
        # the cost is the number of the tokens times the price of a thousand
        # tokens
        self.assertAlmostEqual(summary['total_tokens'] / 1000.0 * 2.0, summary['cost'],
                               places=5)

    def test_the_service_without_an_upstream_is_deterministic(self):
        gateway = self.start()
        status, body = gateway.post('/v1/chat/completions',
                                    {'messages': [{'role': 'user', 'content': 'hello'}]})
        self.assertEqual(200, status)
        self.assertEqual('mock:hello', body['choices'][0]['message']['content'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
