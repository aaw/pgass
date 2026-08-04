#!/usr/bin/env python3
"""Run pgass over the ASP Competition 2015 and 2017 benchmark suites.

The suites are not in this repository. They are downloaded on demand into a
cache directory, which defaults to /tmp/pgass-bench and can be pointed anywhere
with --cache or PGASS_BENCH_CACHE. Nothing is ever written inside the repo.

Every domain here ships an official solution checker.

    bench.py ls
    bench.py ls CrossingMinimization
    bench.py fetch CrossingMinimization
    bench.py run CrossingMinimization --compare
    bench.py run CrossingMinimization 0002
"""

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "benchmarks.json")
DEFAULT_CACHE = os.environ.get("PGASS_BENCH_CACHE", "/tmp/pgass-bench")
DEFAULT_PGASS = os.path.join(REPO, "build", "pgass")

# The exit codes of src/exit_code.h. A run that found something answers with one
# of SATISFIABLE, and one that proved there is nothing answers UNSATISFIABLE.
SATISFIABLE = {10, 30, 62}
UNSATISFIABLE = {20}
NO_RUN = 128
INTERRUPTED = 1

# What pgass cannot read yet. Both 'ls' and 'run' look for these, so that a
# benchmark out of reach is named as such rather than buried among real
# failures.
UNSUPPORTED = [
    ("range", re.compile(r"[0-9A-Za-z_)]\.\.[0-9A-Za-z_(]")),
    ("#show", re.compile(r"#show")),
    ("#const", re.compile(r"#const")),
    ("#minimize", re.compile(r"#minimi[sz]e|#maximi[sz]e")),
]


def load_manifest():
    with open(MANIFEST) as f:
        return json.load(f)


def find_domain(manifest, name):
    for domain in manifest["domains"]:
        if domain["name"].lower() == name.lower():
            return domain
    matches = [d for d in manifest["domains"] if name.lower() in d["name"].lower()]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        sys.exit(f"bench: no such domain '{name}'. Try 'bench.py ls'.")
    names = ", ".join(d["name"] for d in matches)
    sys.exit(f"bench: '{name}' matches several domains: {names}")


def domain_dir(cache, domain):
    return os.path.join(cache, str(domain["year"]), domain["name"])


def checker_dir(cache, name):
    return os.path.join(cache, "checkers", name)


def human(size):
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f}{unit}" if unit == "B" else f"{size:.1f}{unit}"
        size /= 1024


# ---------------------------------------------------------------- fetching


def download(url, dest):
    """Downloads to a temporary name and renames, so that an interrupted
    download never leaves behind a file that looks complete."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    with urllib.request.urlopen(url) as response, open(tmp, "wb") as out:
        shutil.copyfileobj(response, out)
    os.replace(tmp, dest)
    return dest


def unzip_into(archive, dest):
    """Unpacks `archive`, keeping the permissions recorded in it. Python drops
    them otherwise, which would leave the checker scripts unrunnable."""
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            path = z.extract(entry, dest)
            mode = entry.external_attr >> 16
            if mode:
                os.chmod(path, mode)


def fetch_domain(cache, domain, force=False):
    target = domain_dir(cache, domain)
    if os.path.isdir(target) and not force:
        print(f"  {domain['name']}: already fetched")
        return
    if force and os.path.isdir(target):
        shutil.rmtree(target)

    print(f"  {domain['name']}: downloading {human(domain['size'])}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = download(domain["url"], os.path.join(tmp, "domain.zip"))
        # A 2015 zip holds one top-level folder named for the domain, so it
        # unpacks into the year directory. A 2017 zip has no such folder, so it
        # unpacks into the domain directory instead.
        if domain["layout"] == "folder":
            unzip_into(archive, os.path.dirname(target))
        else:
            unzip_into(archive, target)
    if not os.path.isdir(target):
        sys.exit(f"bench: {domain['name']}.zip did not unpack into {target}")


# The solvers a checker calls. claspD is long gone, and clingo answers for it.
BUNDLED_SOLVERS = {"clingo": "clingo", "gringo": "gringo",
                   "clasp": "clasp", "claspD": "clingo"}


def runnable(path):
    try:
        subprocess.run([path, "--version"], capture_output=True, timeout=10)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def relink_solvers(directory):
    """Points a checker's bundled solvers at the ones installed here.

    The checkers ship Linux x86 binaries and call them by path, with stderr sent
    to /dev/null. Left alone on any other platform they fail silently and the
    checker calls a correct answer set wrong, so a checker that cannot run is
    worse than no checker at all.
    """
    replaced, missing = 0, set()
    for root, _, files in os.walk(directory):
        for name in files:
            if name not in BUNDLED_SOLVERS:
                continue
            path = os.path.join(root, name)
            if runnable(path):
                continue
            local = shutil.which(BUNDLED_SOLVERS[name])
            if not local:
                missing.add(BUNDLED_SOLVERS[name])
                continue
            os.remove(path)
            os.symlink(local, path)
            replaced += 1
    return replaced, missing


def fetch_checkers(cache, domains, force=False):
    """Fetches checkers for `domains`. The 2015 ones all live in one zip, so it
    is downloaded once however many domains asked for it."""
    wanted_from_zip = [d for d in domains if d["checker"]["source"] == "checkers.zip"]
    missing = [
        d
        for d in wanted_from_zip
        if force
        or not all(
            os.path.isdir(checker_dir(cache, sub)) for sub in d["checker"]["dirs"]
        )
    ]
    if missing:
        manifest = load_manifest()
        print(f"  checkers.zip: downloading {human(manifest['checkers_zip']['size'])}")
        with tempfile.TemporaryDirectory() as tmp:
            archive = download(
                manifest["checkers_zip"]["url"], os.path.join(tmp, "checkers.zip")
            )
            unzip_into(archive, os.path.join(tmp, "out"))
            source = os.path.join(tmp, "out", "checkers")
            os.makedirs(os.path.join(cache, "checkers"), exist_ok=True)
            for entry in os.listdir(source):
                dest = checker_dir(cache, entry)
                if os.path.isdir(dest):
                    if not force:
                        continue
                    shutil.rmtree(dest)
                shutil.move(os.path.join(source, entry), dest)

    for domain in domains:
        source = domain["checker"]["source"]
        if source == "checkers.zip":
            continue
        dest = checker_dir(cache, domain["name"])
        if os.path.isdir(dest) and not force:
            continue
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        print(f"  {domain['name']} checker: downloading {human(domain['checker']['size'])}")
        with tempfile.TemporaryDirectory() as tmp:
            archive = download(source, os.path.join(tmp, "checker.zip"))
            unzip_into(archive, dest)

    replaced, missing = relink_solvers(os.path.join(cache, "checkers"))
    if replaced:
        print(f"  pointed {replaced} bundled solvers at the ones installed here")
    if missing:
        print(f"  checkers want {', '.join(sorted(missing))}, which is not installed")


# ---------------------------------------------------------------- instances


def instance_roots(cache, domain):
    """The folders holding a domain's instances. 2015 keeps them beside the
    encoding, except CQA which splits into a folder per query. 2017 keeps them
    in a subfolder that is named 'instances' or 'asp_insts' depending on the
    domain."""
    root = domain_dir(cache, domain)
    bases = [os.path.join(root, s) for s in domain.get("subdomains", [])] or [root]
    roots = []
    for base in bases:
        if not os.path.isdir(base):
            continue
        for name in ("instances", "asp_insts"):
            nested = os.path.join(base, name)
            if os.path.isdir(nested):
                roots.append(nested)
                break
        else:
            roots.append(base)
    return roots


def instances_of(cache, domain, selected_only=True):
    """The instance files of a fetched domain. Where the competition published
    which instances it scored, in instances.competition, that is what a run
    defaults to."""
    found = []
    for base in instance_roots(cache, domain):
        listing = os.path.join(base, "instances.competition")
        if selected_only and os.path.exists(listing):
            with open(listing) as f:
                names = [line.strip() for line in f if line.strip()]
            found += [os.path.join(base, n) for n in names]
        else:
            found += [
                os.path.join(base, n)
                for n in sorted(os.listdir(base))
                if n.endswith((".asp", ".lp")) and n != "encoding.asp"
            ]
    return [p for p in found if os.path.isfile(p)]


def encoding_of(cache, domain, instance):
    """The encoding that goes with an instance. It sits beside the instance,
    except where the instances are in a subfolder of their own."""
    beside = os.path.join(os.path.dirname(instance), "encoding.asp")
    if os.path.exists(beside):
        return beside
    return os.path.join(os.path.dirname(os.path.dirname(instance)), "encoding.asp")


def output_predicates(cache, domain, instance, checker=None):
    """The predicates that make up the answer, named as 'steiner/3,edge/4'.

    A checker wants only these, not the whole answer set: the fixtures the
    checkers ship hold nothing but the chosen atoms. pgass has no #show, so
    everything it derives is printed and has to be narrowed down here.
    """
    candidates = [os.path.join(os.path.dirname(instance), "filter.txt"),
                  os.path.join(os.path.dirname(encoding_of(cache, domain, instance)),
                               "filter.txt")]
    if checker:
        candidates.insert(0, os.path.join(checker, "output-predicates.txt"))

    for path in candidates:
        if not os.path.exists(path):
            continue
        with open(path) as f:
            text = f.read().strip()
        names = {item.strip().split("/")[0]
                 for item in text.replace("\n", ",").split(",") if item.strip()}
        if names:
            return names
    return None


def scan_unsupported(paths):
    """The features in these files that pgass cannot read yet."""
    blocking = []
    for path in paths:
        try:
            with open(path, errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for name, pattern in UNSUPPORTED:
            if name not in blocking and pattern.search(text):
                blocking.append(name)
    return blocking


# ---------------------------------------------------------------- running


def parse_pgass(stdout):
    """The cost and the atoms of the first answer set, if there is one."""
    cost, atoms = None, None
    lines = stdout.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("Answer: ") and atoms is None and i + 1 < len(lines):
            atoms = lines[i + 1].split()
        elif line.startswith("Cost:") and cost is None:
            cost = line[len("Cost:"):].split()
    return cost, atoms


def parse_clingo(stdout):
    cost, atoms = None, None
    lines = stdout.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("Answer: ") and i + 1 < len(lines):
            atoms = lines[i + 1].split()
        elif line.startswith("Optimization: "):
            cost = line[len("Optimization: "):].split()
    return cost, atoms


def project(atoms, predicates):
    """Keeps the atoms whose predicate filter.txt asked for."""
    if atoms is None or predicates is None:
        return atoms
    kept = []
    for atom in atoms:
        name = atom.split("(", 1)[0].lstrip("-")
        if name in predicates:
            kept.append(atom)
    return sorted(kept)


def status_of(code):
    if code in SATISFIABLE:
        return "SAT"
    if code in UNSATISFIABLE:
        return "UNSAT"
    if code == NO_RUN:
        return "ERROR"
    if code == INTERRUPTED:
        return "RESOURCE"
    return f"EXIT{code}"


def run_one(args, domain, instance, cache):
    """Solves one instance and, when asked, checks the result against clingo."""
    name = os.path.basename(instance)
    encoding = encoding_of(cache, domain, instance)
    command = [args.pgass, f"--models={args.models}", encoding, instance]
    record = {"domain": domain["name"], "instance": name}

    blocking = scan_unsupported([encoding, instance])
    if blocking:
        record.update(status="SKIP", blocked_by=blocking)
        return record

    cost, atoms = None, None
    started = time.time()
    try:
        done = subprocess.run(command, capture_output=True, text=True,
                              timeout=args.timeout)
    except subprocess.TimeoutExpired:
        done = None
        record.update(status="TIMEOUT", seconds=float(args.timeout))

    if done is not None:
        record["seconds"] = round(time.time() - started, 2)
        record["exit_code"] = done.returncode
        record["status"] = status_of(done.returncode)
        cost, atoms = parse_pgass(done.stdout)
        if cost:
            record["cost"] = cost
        if record["status"] == "ERROR":
            record["error"] = (done.stderr.strip().splitlines() or [""])[0]

    # Asked even when pgass gave up. An instance pgass cannot finish is exactly
    # the one worth knowing clingo's time for.
    if args.compare:
        record["oracle"] = compare_with_clingo(args, encoding, instance, record, cost)
    if atoms is not None:
        record["checker"] = run_checker(args, domain, instance, atoms, record, cache)
    return record


def compare_with_clingo(args, encoding, instance, record, cost):
    """Whether clingo reached the same verdict, and how long it took.

    Only the status and the cost are worth comparing. A program usually has many
    answer sets, so pgass and clingo returning different ones says nothing about
    either. Whether an answer set is right is the checker's question.
    """
    # clingo's own default is one model, and under weak constraints it goes on
    # to prove that model optimal. Asking for '1' outright stops it short of
    # that, which would leave it answering 10 where pgass answers 30 on every
    # optimizing domain. So the count is only passed when it is not the default.
    command = [args.compare, encoding, instance]
    if args.models != 1:
        command.insert(1, str(args.models))
    started = time.time()
    try:
        done = subprocess.run(command, capture_output=True, text=True,
                              timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "seconds": float(args.timeout), "agree": None}
    except FileNotFoundError:
        sys.exit(f"bench: cannot run '{args.compare}'")

    # clingo answers 65 where the standard says 128, so compare the meaning
    # rather than the number.
    theirs = "ERROR" if done.returncode == 65 else status_of(done.returncode)
    their_cost, _ = parse_clingo(done.stdout)
    # Only two settled verdicts can disagree. A pgass that timed out or ran out
    # of ground atoms did not contradict clingo, it failed to answer, and the
    # status column already says so.
    decided = {"SAT", "UNSAT"}
    agree = None
    if record["status"] in decided and theirs in decided:
        agree = theirs == record["status"] and cost == their_cost
    return {
        "seconds": round(time.time() - started, 2),
        "status": theirs,
        "cost": their_cost,
        "agree": agree,
    }


def run_checker(args, domain, instance, atoms, record, cache):
    """Asks the domain's official checker whether the answer set solves the
    instance. This is the only thing that says an answer set is right."""
    names = domain["checker"].get("dirs", [domain["name"]])
    # A sub-domain instance belongs to the checker named for its folder.
    folder = os.path.basename(os.path.dirname(instance))
    directory = checker_dir(cache, folder if folder in names else names[0])
    script = os.path.join(directory, "checker", "checker.sh")
    if not os.path.exists(script):
        return {"result": f"no checker fetched, try 'bench.py fetch {domain['name']}'"}

    predicates = output_predicates(cache, domain, instance,
                                   os.path.dirname(script))
    if predicates is None:
        return {"result": "no output predicates, cannot narrow the answer set"}
    chosen = project(atoms, predicates)
    witness = " ".join(a if a.endswith(".") else a + "." for a in chosen)
    try:
        done = subprocess.run(
            ["sh", script, str(record.get("exit_code", 10)), os.path.abspath(instance)],
            input=witness, capture_output=True, text=True,
            timeout=args.timeout, cwd=os.path.dirname(script),
        )
    except subprocess.TimeoutExpired:
        return {"result": "checker-timeout"}
    output = (done.stdout or "") + (done.stderr or "")
    # A checker that could not do its job still exits 0, so reading the exit
    # code alone would take its silence for a pass. Anything that looks like a
    # complaint from the checker or the solver under it means there is no
    # verdict here to believe.
    lowered = output.lower()
    if "exec format error" in lowered or "cannot execute" in lowered:
        return {"result": "checker ships Linux binaries that will not run here"}
    if "#hide" in output:
        # #hide went away in clingo 5, and a third of these checkers were
        # written against clingo 3. Rewriting their logic to suit is not this
        # script's business.
        return {"result": "checker needs a clingo older than 5"}
    if ("error" in lowered or "Internal Checker-Error" in output
            or "WARN" in output):
        return {"result": "checker could not run"}
    return {"result": "ok" if done.returncode == 0 else "rejected"}


# ---------------------------------------------------------------- commands


def domain_status(cache, domain, instances):
    """What stands between pgass and a domain. Reading every instance of a large
    domain would be slow and tells you nothing the first few do not, so this
    reads the encoding and a handful of instances."""
    sample = instances[:5]
    encodings = {encoding_of(cache, domain, i) for i in sample}
    blocking = scan_unsupported(sorted(encodings) + sample)
    return "needs " + ", ".join(blocking) if blocking else "runnable"


def match_instances(instances, pattern):
    """The instances whose name the pattern picks out. Naming an instance in
    full is a mouthful, so any part of the name will do, as will a glob."""
    if not any(c in pattern for c in "*?["):
        pattern = f"*{pattern}*"
    return [i for i in instances if fnmatch.fnmatch(os.path.basename(i), pattern)]


def list_instances(args, domain):
    """The instances of one domain, which is what naming a domain asks for.
    Paths are printed in full, next to the encoding each one is solved with, so
    that a line can be pasted straight into a pgass command."""
    if not os.path.isdir(domain_dir(args.cache, domain)):
        sys.exit(f"bench: {domain['name']} is not fetched. "
                 f"Try 'bench.py fetch {domain['name']}'.")

    instances = instances_of(args.cache, domain,
                             selected_only=not args.all_instances)
    if not instances:
        sys.exit(f"bench: {domain['name']} has no instances")
    if args.instance:
        instances = match_instances(instances, args.instance)
        if not instances:
            sys.exit(f"bench: no instance of {domain['name']} matches "
                     f"'{args.instance}'")

    width = max(len(p) for p in instances)
    for path in instances:
        print(f"{path:<{width}} {human(os.path.getsize(path)):>9}")

    encodings = sorted({encoding_of(args.cache, domain, i) for i in instances})
    for path in encodings:
        if os.path.exists(path):
            print(f"\nencoding: {path}")

    scored = "" if args.all_instances else ", the ones the competition scored"
    print(f"\n{len(instances)} instances of {domain['name']}{scored}.")


def cmd_ls(args):
    manifest = load_manifest()
    if args.domain:
        return list_instances(args, find_domain(manifest, args.domain))

    rows = []
    for domain in manifest["domains"]:
        fetched = os.path.isdir(domain_dir(args.cache, domain))
        if args.fetched and not fetched:
            continue
        count, status = "-", ""
        if fetched:
            instances = instances_of(args.cache, domain)
            count = len(instances)
            status = domain_status(args.cache, domain, instances)
        rows.append((domain["name"], domain["year"], human(domain["size"]),
                     count, status))

    print(f"{'DOMAIN':<46} {'YEAR':>4} {'SIZE':>9} {'N':>5}  STATUS")
    for name, year, size, count, status in rows:
        print(f"{name:<46} {year:>4} {size:>9} {count:>5}  {status}")


def cmd_fetch(args):
    manifest = load_manifest()
    if args.all:
        domains = manifest["domains"]
    elif args.domains:
        domains = [find_domain(manifest, n) for n in args.domains]
    else:
        sys.exit("bench: name a domain, or pass --all")

    print(f"Fetching {len(domains)} domains into {args.cache}")
    for domain in domains:
        fetch_domain(args.cache, domain, force=args.force)
    fetch_checkers(args.cache, domains, force=args.force)
    print("Done.")


def cmd_run(args):
    manifest = load_manifest()
    if not os.path.exists(args.pgass):
        sys.exit(f"bench: no pgass at {args.pgass}. Run 'make' first.")

    domain = find_domain(manifest, args.domain)
    if not os.path.isdir(domain_dir(args.cache, domain)):
        sys.exit(f"bench: {domain['name']} not fetched. "
                 f"Try 'bench.py fetch {domain['name']}'.")

    instances = instances_of(args.cache, domain, selected_only=not args.all_instances)
    if args.instance:
        instances = match_instances(instances, args.instance)
        if not instances:
            sys.exit(f"bench: no instance of {domain['name']} matches "
                     f"'{args.instance}'")
    if not instances:
        sys.exit(f"bench: {domain['name']} has no instances")

    print(f"{domain['name']}: {len(instances)} instances, {args.timeout}s timeout\n")
    header = f"{'INSTANCE':<44} {'PGASS':<18}"
    if args.compare:
        header += f"  {args.compare.upper():<18}"
    print(header)

    # One at a time, so that the seconds reported mean something. Solvers racing
    # each other for the same cores would not be timing anything.
    records = []
    interrupted = False
    for instance in instances:
        # The name goes out before the instance is solved, leaving the line open
        # for its result. A run sitting on a hard instance for its whole timeout
        # then says which one it is.
        print(f"{os.path.basename(instance):<44} ", end="", flush=True)
        try:
            record = run_one(args, domain, instance, args.cache)
        except KeyboardInterrupt:
            print("\nCancelled.", flush=True)
            interrupted = True
            break
        records.append(record)
        report_row(record, args.compare)
    if records:
        summarize(records)
    if interrupted:
        sys.exit(130)


def timing(status, seconds):
    return f"{status:<8} {seconds:>8.2f}s" if seconds is not None else f"{status:<18}"


def report_row(record, comparing):
    """Finishes the line begun with the instance name."""
    row = timing(record["status"], record.get("seconds"))
    if comparing:
        oracle = record.get("oracle") or {}
        row += "  " + timing(oracle.get("status", "-"), oracle.get("seconds"))
    if record.get("cost"):
        row += f"  cost {' '.join(record['cost'])}"
    if record.get("blocked_by"):
        row += f"  needs {', '.join(record['blocked_by'])}"
    if (record.get("checker") or {}).get("result") == "rejected":
        row += "  CHECKER REJECTED"
    if record.get("error"):
        row += f"  {record['error']}"
    print(row)


def summarize(records):
    counts = {}
    for record in records:
        counts[record["status"]] = counts.get(record["status"], 0) + 1
    print("\n" + "  ".join(f"{status}={n}" for status, n in sorted(counts.items())))

    # An oracle that ran out of time did not disagree, it just did not answer.
    disagreed = [r for r in records
                 if (r.get("oracle") or {}).get("agree") is False]
    if disagreed:
        print(f"{len(disagreed)} disagree with the oracle")
    verdicts = {}
    for record in records:
        result = (record.get("checker") or {}).get("result")
        if result:
            verdicts[result] = verdicts.get(result, 0) + 1
    for result, n in sorted(verdicts.items(), key=lambda kv: -kv[1]):
        print(f"{n} {result}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", default=DEFAULT_CACHE,
                        help=f"where suites are downloaded (default {DEFAULT_CACHE})")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ls", help="list the domains, or the instances of one")
    p.add_argument("domain", nargs="?", help="list this domain's instances")
    p.add_argument("instance", nargs="?",
                   help="any part of an instance name, or a glob")
    p.add_argument("--fetched", action="store_true", help="only what is downloaded")
    p.add_argument("--all-instances", action="store_true",
                   help="every instance, not just the ones the competition scored")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("fetch", help="download domains into the cache")
    p.add_argument("domains", nargs="*")
    p.add_argument("--all", action="store_true")
    p.add_argument("--force", action="store_true", help="re-download what is there")
    p.set_defaults(func=cmd_fetch)

    p = sub.add_parser("run", help="solve a domain, or one instance of it")
    p.add_argument("domain")
    p.add_argument("instance", nargs="?",
                   help="any part of an instance name, or a glob")
    p.add_argument("--pgass", default=DEFAULT_PGASS)
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument("--models", type=int, default=1)
    p.add_argument("--all-instances", action="store_true",
                   help="every instance, not just the ones the competition scored")
    p.add_argument("--compare", nargs="?", const="clingo", default=None,
                   metavar="SOLVER", help="check the verdict against clingo")
    p.set_defaults(func=cmd_run)

    args = parser.parse_args()
    if getattr(args, "compare", None) and not shutil.which(args.compare):
        sys.exit(f"bench: --compare asked for '{args.compare}', which is not installed")
    try:
        args.func(args)
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        sys.exit(130)


if __name__ == "__main__":
    main()
