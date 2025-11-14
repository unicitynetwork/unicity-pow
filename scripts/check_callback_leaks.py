#!/usr/bin/env python3
"""
Callback Memory Leak Detection for Unicity

Detects potential memory leaks caused by:
1. Callbacks capturing shared_from_this()
2. Asymmetric cleanup paths (some clear callbacks, some don't)

Usage:
    ./scripts/check_callback_leaks.py

Returns:
    0 if no issues found
    1 if potential issues detected
"""

import re
import sys
from pathlib import Path
from collections import defaultdict
from typing import List, Dict, Optional

class Colors:
    """ANSI color codes for terminal output"""
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    MAGENTA = '\033[95m'
    CYAN = '\033[96m'
    BOLD = '\033[1m'
    END = '\033[0m'

def colorize(text: str, color: str) -> str:
    """Wrap text in color codes"""
    return f"{color}{text}{Colors.END}"

def find_shared_from_this_captures(file_path: Path) -> List[Dict]:
    """
    Find lambdas capturing shared_from_this().

    Returns list of dictionaries with:
    - file: Path object
    - line: line number
    - var: variable name
    - context: surrounding code snippet
    """
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Warning: Could not read {file_path}: {e}")
        return []

    # Pattern: auto/PeerPtr self = shared_from_this()
    pattern = r'(?:auto|PeerPtr|std::shared_ptr<\w+>)\s+(\w+)\s*=\s*shared_from_this\(\)'
    matches = re.finditer(pattern, content, re.MULTILINE)

    captures = []
    for match in matches:
        var_name = match.group(1)
        line_num = content[:match.start()].count('\n') + 1

        # Look for lambda captures using this variable within next 500 chars
        lambda_pattern = rf'\[(?:[^\]]*\b{var_name}\b[^\]]*)\]'
        lambda_start = match.end()
        lambda_search = re.search(lambda_pattern, content[lambda_start:lambda_start+500])

        if lambda_search:
            # Extract context (3 lines)
            lines = content[:match.start()].split('\n')
            start_line = max(0, len(lines) - 2)
            context_lines = content.split('\n')[start_line:line_num+2]
            context = '\n'.join(f"    {i+start_line+1:4d}: {line}"
                               for i, line in enumerate(context_lines))

            captures.append({
                'file': file_path,
                'line': line_num,
                'var': var_name,
                'context': context
            })

    return captures

def find_callback_methods(file_path: Path) -> Dict[str, List[Dict]]:
    """
    Find methods that set callbacks.

    Returns dictionary mapping object_name to list of callback info dicts.
    """
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Warning: Could not read {file_path}: {e}")
        return {}

    # Pattern: object->set_*_callback(...)
    pattern = r'(\w+)->set_(\w+)_callback\('
    matches = re.finditer(pattern, content, re.MULTILINE)

    callbacks = defaultdict(list)
    for match in matches:
        obj_name = match.group(1)
        callback_type = match.group(2)
        line_num = content[:match.start()].count('\n') + 1
        callbacks[obj_name].append({
            'type': callback_type,
            'line': line_num,
            'file': file_path
        })

    return dict(callbacks)

def find_cleanup_paths(file_path: Path) -> List[Dict]:
    """
    Find cleanup methods that might clear callbacks.

    Returns list of dictionaries with:
    - method: method name
    - line: line number
    - clears_callbacks: bool
    - file: Path object
    """
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"Warning: Could not read {file_path}: {e}")
        return []

    cleanup_methods = []

    # Methods that might be cleanup paths
    cleanup_patterns = [
        r'void\s+(disconnect|close|cleanup|shutdown|stop|do_disconnect)\s*\(',
        r'~(\w+)\s*\('  # Destructors
    ]

    for i, line in enumerate(lines):
        for pattern in cleanup_patterns:
            match = re.search(pattern, line)
            if match:
                method_name = match.group(1)

                # Check if this method clears callbacks (look ahead 100 lines)
                clears_callbacks = False
                for j in range(i, min(i+100, len(lines))):
                    if 'set_receive_callback({})' in lines[j] or \
                       'set_disconnect_callback({})' in lines[j] or \
                       'receive_callback_ = {}' in lines[j] or \
                       'disconnect_callback_ = {}' in lines[j]:
                        clears_callbacks = True
                        break

                cleanup_methods.append({
                    'method': method_name,
                    'line': i + 1,
                    'clears_callbacks': clears_callbacks,
                    'file': file_path
                })
                break  # Only match first pattern per line

    return cleanup_methods

def check_asymmetric_cleanup(cleanup_paths: List[Dict]) -> Optional[Dict]:
    """
    Detect if some cleanup paths clear callbacks while others don't.

    Returns dict with warning info if asymmetry detected, None otherwise.
    """
    if not cleanup_paths:
        return None

    clearing = [p for p in cleanup_paths if p['clears_callbacks']]
    not_clearing = [p for p in cleanup_paths if not p['clears_callbacks']]

    # If we have both types, that's asymmetric
    if clearing and not_clearing:
        return {
            'warning': 'ASYMMETRIC CLEANUP DETECTED',
            'clearing': clearing,
            'not_clearing': not_clearing
        }

    return None

def analyze_file(file_path: Path, verbose: bool = True) -> Dict:
    """
    Analyze a single file for callback leak patterns.

    Returns dictionary with analysis results.
    """
    results = {
        'captures': [],
        'callbacks': {},
        'cleanup_paths': [],
        'asymmetry': None
    }

    if verbose:
        print(f"\n{colorize(f'=== Analyzing {file_path.relative_to(Path.cwd())} ===', Colors.CYAN)}")

    # Check for shared_from_this captures
    captures = find_shared_from_this_captures(file_path)
    if captures:
        results['captures'] = captures
        if verbose:
            print(f"\n{colorize('⚠️  Found', Colors.YELLOW)} {colorize(str(len(captures)), Colors.BOLD)} {colorize('shared_from_this() captures:', Colors.YELLOW)}")
            for cap in captures:
                print(f"  {colorize('Line', Colors.BLUE)} {colorize(str(cap['line']), Colors.BOLD)}: variable '{colorize(cap['var'], Colors.MAGENTA)}'")

    # Check for callback methods
    callbacks = find_callback_methods(file_path)
    if callbacks:
        results['callbacks'] = callbacks
        if verbose:
            print(f"\n{colorize('📞 Found callback setters:', Colors.CYAN)}")
            for obj, calls in callbacks.items():
                print(f"  {colorize(obj, Colors.MAGENTA)}: {len(calls)} callback(s) set")

    # Check cleanup paths
    cleanup_paths = find_cleanup_paths(file_path)
    if cleanup_paths:
        results['cleanup_paths'] = cleanup_paths
        if verbose:
            print(f"\n{colorize('🧹 Found', Colors.CYAN)} {colorize(str(len(cleanup_paths)), Colors.BOLD)} {colorize('cleanup methods:', Colors.CYAN)}")
            for path in cleanup_paths:
                status = f"{colorize('✅ clears callbacks', Colors.GREEN)}" if path['clears_callbacks'] \
                    else f"{colorize('❌ DOES NOT clear callbacks', Colors.RED)}"
                print(f"  {colorize(path['method'] + '()', Colors.MAGENTA)} at line {colorize(str(path['line']), Colors.BOLD)}: {status}")

        # Check for asymmetry
        asymmetry = check_asymmetric_cleanup(cleanup_paths)
        if asymmetry:
            results['asymmetry'] = asymmetry
            if verbose:
                print(f"\n{colorize('❗ ' + asymmetry['warning'], Colors.RED + Colors.BOLD)}")
                print(f"  {colorize('Methods that clear callbacks:', Colors.GREEN)}")
                for m in asymmetry['clearing']:
                    print(f"    - {colorize(m['method'] + '()', Colors.MAGENTA)} at line {colorize(str(m['line']), Colors.BOLD)}")
                print(f"  {colorize('Methods that DO NOT clear callbacks:', Colors.RED)}")
                for m in asymmetry['not_clearing']:
                    print(f"    - {colorize(m['method'] + '()', Colors.MAGENTA)} at line {colorize(str(m['line']), Colors.BOLD)}")

    return results

def main():
    """Main entry point"""
    print(colorize("=" * 70, Colors.BOLD))
    print(colorize("Unicity Callback Memory Leak Detection", Colors.BOLD + Colors.CYAN))
    print(colorize("=" * 70, Colors.BOLD))

    # Find all C++ source and header files
    base_path = Path.cwd()
    source_files = list((base_path / 'src' / 'network').glob('*.cpp'))
    header_files = list((base_path / 'include' / 'network').glob('*.hpp'))
    test_files = list((base_path / 'test' / 'network').glob('**/*.cpp')) + \
                 list((base_path / 'test' / 'network').glob('**/*.hpp'))

    all_files = source_files + header_files + test_files

    if not all_files:
        print(colorize("\n❌ No source files found. Are you in the project root?", Colors.RED))
        return 1

    print(f"\n{colorize('Scanning', Colors.CYAN)} {colorize(str(len(all_files)), Colors.BOLD)} {colorize('files...', Colors.CYAN)}\n")

    # Analyze all files
    all_issues = []
    files_with_issues = []

    for file_path in sorted(all_files):
        results = analyze_file(file_path, verbose=True)

        # Check if this file has any issues
        has_issues = bool(results['captures'] or results['asymmetry'])
        if has_issues:
            files_with_issues.append(file_path)
            all_issues.append(results)

    # Summary
    print(f"\n{colorize('=' * 70, Colors.BOLD)}")
    if all_issues:
        print(colorize(f"⚠️  SUMMARY: Found potential issues in {len(files_with_issues)} file(s)",
                      Colors.YELLOW + Colors.BOLD))
        print(colorize("=" * 70, Colors.BOLD))

        print(f"\n{colorize('Files with issues:', Colors.YELLOW)}")
        for f in files_with_issues:
            print(f"  - {f.relative_to(base_path)}")

        print(f"\n{colorize('Recommended actions:', Colors.CYAN)}")
        print("  1. Review all shared_from_this() captures")
        print("  2. Ensure callbacks are cleared on ALL cleanup paths")
        print("  3. Fix any asymmetric cleanup patterns")

        return 1
    else:
        print(colorize("✅ SUCCESS: No callback leak patterns detected!",
                      Colors.GREEN + Colors.BOLD))
        print(colorize("=" * 70, Colors.BOLD))
        return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print(f"\n\n{colorize('Interrupted by user', Colors.YELLOW)}")
        sys.exit(130)
    except Exception as e:
        print(f"\n{colorize('ERROR:', Colors.RED + Colors.BOLD)} {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
