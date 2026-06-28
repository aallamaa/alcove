#!/usr/bin/env python3
import re
import subprocess
import sys
import os

def run_repl(dialect, code):
    # Set up executable path
    exe = "./alcove" if dialect == "alcove" else "./adder"
    if not os.path.exists(exe):
        # Fallback if adder is not built or symlinked
        exe = "./alcove"
    
    # We pipe code into the REPL to get interactive prompts
    cmd = [exe, "-n"]
    if dialect == "adder" and exe == "./alcove":
        # If running adder code via alcove binary (which doesn't happen usually, but for safety)
        cmd = ["./adder", "-n"]
        
    try:
        proc = subprocess.run(cmd, input=code, capture_output=True, text=True, timeout=5)
        stdout = proc.stdout
    except subprocess.TimeoutExpired as e:
        stdout = e.stdout or ""
        stdout += "\n[Error: Execution timed out]"
    
    # Strip ANSI color escape sequences
    ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
    stdout = ansi_escape.sub('', stdout)

    # Normalize hex addresses and pointers (e.g. @5595fedaaa00 or 0x5595fedaaa00 -> @...)
    stdout = re.sub(r'0x[0-9a-fA-F]+|@[0-9a-fA-F]+', '@...', stdout)
    
    # Strip trailing empty prompts (like the final In [X]: prompt)
    lines = stdout.splitlines()
    clean_lines = []
    for line in lines:
        # If it's a bare trailing prompt like "In [2]:" or "In [12]:", skip it if it's the last lines
        clean_lines.append(line)
        
    while clean_lines and re.match(r'^In \[\d+\]:\s*$', clean_lines[-1]):
        clean_lines.pop()
        
    return "\n".join(clean_lines).strip()

def build_book(template_path, output_path):
    if not os.path.exists(template_path):
        print(f"Error: Template {template_path} not found.")
        sys.exit(1)
        
    with open(template_path, "r", encoding="utf-8") as f:
        content = f.read()
        
    # We find blocks of the form:
    # <!-- exec: <dialect> -->
    # ```
    # <code>
    # ```
    pattern = re.compile(
        r'<!--\s*exec:\s*(alcove|adder)\s*-->\s*```(?:[a-zA-Z0-9_-]+)?\n(.*?)\n```',
        re.DOTALL
    )
    
    def replacer(match):
        dialect = match.group(1)
        code = match.group(2)
        print(f"Executing {dialect} code block...")
        repl_output = run_repl(dialect, code)
        
        # Format the result as a REPL session block
        return f"<!-- exec: {dialect} -->\n```\n{repl_output}\n```"
        
    new_content = pattern.sub(replacer, content)
    
    # Save the compiled book
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(new_content)
        
    print(f"Book compiled successfully and saved to {output_path}")

if __name__ == "__main__":
    template = "docs/book_template.md"
    output = "docs/alcove-adder-book.md"
    if len(sys.argv) > 1:
        template = sys.argv[1]
    if len(sys.argv) > 2:
        output = sys.argv[2]
    build_book(template, output)
