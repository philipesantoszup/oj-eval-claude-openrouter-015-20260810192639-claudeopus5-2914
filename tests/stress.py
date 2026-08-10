#!/usr/bin/env python3
"""Randomized differential test for the on-disk key-value store.

Runs the compiled `code` binary in several consecutive sessions against the
same database file and compares its output with an in-memory reference model.
"""
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "code")
DB = os.path.join(ROOT, "storage.db")


def run_case(seed, sessions, ops_per_session, key_pool, val_pool, verbose=False):
    rng = random.Random(seed)
    if os.path.exists(DB):
        os.remove(DB)
    model = {}
    keys = ["k%d" % i for i in range(key_pool)]
    # exercise the 64-byte boundary too
    keys.append("z" * 64)
    keys.append("a" * 64)

    for s in range(sessions):
        cmds = []
        expected = []
        for _ in range(ops_per_session):
            r = rng.random()
            idx = rng.choice(keys)
            if r < 0.45:
                v = rng.randrange(val_pool)
                cmds.append("insert %s %d" % (idx, v))
                model.setdefault(idx, set()).add(v)
            elif r < 0.75:
                if rng.random() < 0.6 and model.get(idx):
                    v = rng.choice(sorted(model[idx]))
                else:
                    v = rng.randrange(val_pool)
                cmds.append("delete %s %d" % (idx, v))
                if idx in model:
                    model[idx].discard(v)
                    if not model[idx]:
                        del model[idx]
            else:
                cmds.append("find %s" % idx)
                vals = sorted(model.get(idx, []))
                expected.append(" ".join(map(str, vals)) if vals else "null")

        inp = "%d\n%s\n" % (len(cmds), "\n".join(cmds))
        out = subprocess.run([BIN], input=inp, capture_output=True, text=True,
                             cwd=ROOT)
        if out.returncode != 0:
            print("seed %d session %d: exit code %d" % (seed, s, out.returncode))
            print(out.stderr[:2000])
            return False
        got = out.stdout.split("\n")
        got = [l for l in got if l != ""] if got and got[-1] == "" else got
        if got != expected:
            print("seed %d session %d MISMATCH" % (seed, s))
            for i, (g, e) in enumerate(zip(got, expected)):
                if g != e:
                    print("  line %d: got %r want %r" % (i, g[:120], e[:120]))
                    break
            print("  got %d lines, want %d" % (len(got), len(expected)))
            with open(os.path.join(ROOT, "tests", "fail.in"), "w") as f:
                f.write(inp)
            return False
    return True


def main():
    configs = [
        # sessions, ops, key pool, value pool
        (3, 300, 4, 30),      # heavy duplication, many splits/merges per key
        (2, 800, 2, 2000),    # one huge index -> long leaf chains
        (5, 400, 60, 100),
        (4, 500, 1000, 1000000),
        (2, 2000, 15, 50),    # lots of deletes hitting the same subtrees
        (6, 250, 3, 8),       # tiny value space -> constant re-insert/delete
    ]
    ok = True
    for seed in range(int(sys.argv[1]) if len(sys.argv) > 1 else 12):
        cfg = configs[seed % len(configs)]
        if not run_case(seed, *cfg):
            ok = False
            break
        print("seed %d ok (%s)" % (seed, cfg))
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


# --- extra targeted scenarios, run via: python3 stress.py --extra ---
def run_script(cmds_sessions, label):
    """cmds_sessions: list of command lists; validates against a model."""
    if os.path.exists(DB):
        os.remove(DB)
    model = {}
    for si, cmds in enumerate(cmds_sessions):
        expected = []
        for c in cmds:
            p = c.split()
            if p[0] == "insert":
                model.setdefault(p[1], set()).add(int(p[2]))
            elif p[0] == "delete":
                if p[1] in model:
                    model[p[1]].discard(int(p[2]))
                    if not model[p[1]]:
                        del model[p[1]]
            else:
                v = sorted(model.get(p[1], []))
                expected.append(" ".join(map(str, v)) if v else "null")
        inp = "%d\n%s\n" % (len(cmds), "\n".join(cmds))
        r = subprocess.run([BIN], input=inp, capture_output=True, text=True, cwd=ROOT)
        got = [l for l in r.stdout.split("\n") if l != ""]
        if r.returncode != 0 or got != expected:
            print("%s session %d FAILED (rc=%d)" % (label, si, r.returncode))
            for i, (g, e) in enumerate(zip(got, expected)):
                if g != e:
                    print("  line %d: got %r want %r" % (i, g[:100], e[:100]))
                    break
            print("  got %d lines want %d" % (len(got), len(expected)))
            return False
    print("%s ok" % label)
    return True


def extra():
    ok = True
    rng = random.Random(99)

    # grow to a tall tree, delete every record, then regrow (root collapse)
    ins = ["insert k%05d %d" % (i, i) for i in range(3000)]
    dele = ["delete k%05d %d" % (i, i) for i in range(3000)]
    probe = ["find k00000", "find k01500", "find k02999"]
    ok &= run_script([ins + probe, dele + probe, ins[:50] + probe], "grow/erase/regrow")

    # single index with a long value list, deletions from the middle
    a = ["insert one %d" % i for i in range(2000)]
    b = ["delete one %d" % i for i in range(500, 1500)]
    ok &= run_script([a + ["find one"], b + ["find one"],
                      ["insert one 700", "insert one 1200", "find one"]], "long chain")

    # reverse-order insertion (always updates the leftmost separator)
    rev = ["insert r%05d %d" % (i, 5) for i in range(2000)][::-1]
    ok &= run_script([rev + ["find r00000", "find r01999"]], "reverse insert")

    # deleting keys that never existed, interleaved with real work
    mix = []
    for i in range(3000):
        if i % 3 == 0:
            mix.append("insert m%04d %d" % (rng.randrange(400), rng.randrange(50)))
        elif i % 3 == 1:
            mix.append("delete m%04d %d" % (rng.randrange(4000), rng.randrange(50)))
        else:
            mix.append("find m%04d" % rng.randrange(600))
    ok &= run_script([mix, mix, mix], "phantom deletes")

    # boundary-length indices and extreme values
    big = "Z" * 64
    small = "Z" * 63
    cmds = ["insert %s 0" % big, "insert %s 2147483647" % big,
            "insert %s 1" % small, "insert %s 2147483646" % small,
            "find %s" % big, "find %s" % small,
            "delete %s 0" % big, "find %s" % big]
    ok &= run_script([cmds], "boundaries")
    print("EXTRA ALL OK" if ok else "EXTRA FAILED")
    return ok


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--extra":
        sys.exit(0 if extra() else 1)
    sys.exit(main())
