#!/usr/bin/env python3
"""imgmeta works with and without git; the two sidecars differ only in the id."""
import importlib.util, json, os, subprocess, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def load(root):
    spec = importlib.util.spec_from_file_location('imgmeta', os.path.join(HERE, 'imgmeta.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.ROOT = root
    return m


def tree(d, git):
    os.makedirs(os.path.join(d, 'shim'))
    open(os.path.join(d, 'shim', 'a.c'), 'w').write('x')
    open(os.path.join(d, 'img'), 'wb').write(b'img')
    if git:
        run = lambda *c: subprocess.check_call(['git', '-c', 'user.name=t', '-c', 'user.email=t@t', *c], cwd=d, stdout=subprocess.DEVNULL)
        run('init', '-q'); run('add', 'shim'); run('commit', '-qm', 'x')


class T(unittest.TestCase):
    def meta(self, git):
        with tempfile.TemporaryDirectory() as d:
            tree(d, git)
            m = load(d)
            a = type('A', (), dict(img=os.path.join(d, 'img'), kind='probe', args=''))
            m.write(a)
            ck = type('C', (), dict(img=a.img, expect='probe', allow_stale=False, max_age_min=0))
            self.assertEqual(m.check(ck), 0)
            open(os.path.join(d, 'shim', 'a.c'), 'w').write('y')
            self.assertEqual(m.check(ck), 1)  # stale is still refused, git or not
            return json.load(open(a.img + '.meta.json'))

    def test_same_but_id(self):
        n, g = self.meta(False), self.meta(True)
        self.assertEqual(n['git_head'], 'nogit')
        self.assertNotEqual(g['git_head'], 'nogit')
        for k in ('git_head', 'built_at'):
            n.pop(k); g.pop(k)
        self.assertEqual(n, g)


if __name__ == '__main__':
    unittest.main()
