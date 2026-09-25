from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from inspect_dump import inspect

watch, child = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='x4vr-crash-watch-') as temp:
    output = Path(temp)/'report'
    result = subprocess.run([watch, child, str(output)], timeout=30, capture_output=True, text=True)
    assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
    events = (output/'debug-events.log').read_text()
    assert 'first=1 code=0xe0425844' in events, events
    assert 'first=0 code=0xe0425844' in events, events
    assert 'dump success=1 context=1' in events, events
    assert 'x4vr diagnostic fixture unicode' in events, events
    dump = next(output.glob('crash-*.dmp')).read_bytes()
    assert dump[:4] == b'MDMP'
    count, directory = struct.unpack_from('<II', dump, 8)
    exception_found = False
    for i in range(count):
        kind, size, rva = struct.unpack_from('<III', dump, directory+i*12)
        if kind == 6:  # MINIDUMP_EXCEPTION_STREAM
            assert struct.unpack_from('<I', dump, rva+8)[0] == 0xe0425844
            exception_found = True
    assert exception_found
    summary = inspect(next(output.glob('crash-*.dmp')))
    assert summary['exception']['code'] == '0xe0425844'
    assert summary['exception']['thread'] > 0
    assert 'rip' in summary['exception']['registers']
print('External recorder preserves first-chance handling and captures terminal exception context')
