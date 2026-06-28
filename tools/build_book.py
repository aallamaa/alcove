#!/usr/bin/env python3
import re
import subprocess
import sys
import os

def split_lisp_forms(code):
    forms = []
    current = []
    depth = 0
    in_string = False
    escape = False
    for char in code:
        if escape:
            current.append(char)
            escape = False
            continue
        if char == '\\':
            current.append(char)
            escape = True
            continue
        if char == '"':
            in_string = not in_string
            current.append(char)
            continue
        if not in_string:
            if char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
        current.append(char)
        if depth == 0 and not in_string and char in ('\n', ' ', '\t'):
            form = "".join(current).strip()
            if form:
                forms.append(form)
            current = []
    form = "".join(current).strip()
    if form:
        forms.append(form)
    return [f for f in forms if f]

def split_adder_forms(code):
    lines = code.splitlines()
    forms = []
    current = []
    for line in lines:
        if not line.strip():
            if current and not current[-1].strip().endswith(':'):
                forms.append("\n".join(current))
                current = []
            continue
        is_indented = line.startswith(" ") or line.startswith("\t")
        if is_indented:
            current.append(line)
        else:
            if current:
                if not current[-1].strip().endswith(':'):
                    forms.append("\n".join(current))
                    current = []
            current.append(line)
    if current:
        forms.append("\n".join(current))
    return [f for f in forms if f]

def run_repl_session(dialect, forms):
    exe = "./alcove" if dialect == "alcove" else "./adder"
    if not os.path.exists(exe):
        exe = "./alcove"
    
    cmd = [exe, "-n"]
    if dialect == "adder" and exe == "./alcove":
        cmd = ["./adder", "-n"]

    full_input = "\n".join(forms) + "\n"
    
    try:
        proc = subprocess.run(cmd, input=full_input, capture_output=True, text=True, timeout=10)
        stdout = proc.stdout
    except subprocess.TimeoutExpired as e:
        stdout = e.stdout or ""
        stdout += "\n[Error: Execution timed out]"

    ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
    stdout = ansi_escape.sub('', stdout)
    stdout = re.sub(r'[\x01\x02]', '', stdout)
    stdout = re.sub(r'0x[0-9a-fA-F]+|@[0-9a-fA-F]+', '@...', stdout)

    sections = re.split(r'In \[\d+\]:', stdout)
    responses = [s.strip() for s in sections[1:]]
    return responses

def format_repl_markdown(dialect, forms, responses):
    formatted = []
    lang = "clojure" if dialect == "alcove" else "python"
    
    for i, form in enumerate(forms):
        idx = i + 1
        formatted.append(f"**In [{idx}]:**")
        formatted.append(f"```{lang}\n{form}\n```")
        
        if i < len(responses):
            response = responses[i]
            marker = f"Out[{idx}]:"
            if marker in response:
                parts = response.split(marker, 1)
                stdout_part = parts[0].strip()
                retval_part = parts[1].strip()
                
                if stdout_part:
                    formatted.append("*Stdout:*")
                    formatted.append(f"```text\n{stdout_part}\n```")
                if retval_part:
                    formatted.append(f"**Out [{idx}]:**")
                    formatted.append(f"```text\n{retval_part}\n```")
            else:
                alt_marker = re.search(r'Out\[\d+\]:', response)
                if alt_marker:
                    parts = response.split(alt_marker.group(0), 1)
                    stdout_part = parts[0].strip()
                    retval_part = parts[1].strip()
                    if stdout_part:
                        formatted.append("*Stdout:*")
                        formatted.append(f"```text\n{stdout_part}\n```")
                    if retval_part:
                        formatted.append(f"**Out [{idx}]:**")
                        formatted.append(f"```text\n{retval_part}\n```")
                else:
                    if response:
                        formatted.append(f"**Out [{idx}]:**")
                        formatted.append(f"```text\n{response}\n```")
        else:
            formatted.append(f"**Out [{idx}]:**")
            formatted.append("```text\n[No response received]\n```")
            
        formatted.append("")
        
    return "\n".join(formatted).strip()

def build_book(template_path, output_path):
    if not os.path.exists(template_path):
        print(f"Error: Template {template_path} not found.")
        sys.exit(1)
        
    with open(template_path, "r", encoding="utf-8") as f:
        content = f.read()
        
    pattern = re.compile(
        r'<!--\s*exec:\s*(alcove|adder)\s*-->\s*```(?:[a-zA-Z0-9_-]+)?\n(.*?)\n```',
        re.DOTALL
    )
    
    def replacer(match):
        dialect = match.group(1)
        code = match.group(2)
        
        if dialect == "alcove":
            forms = split_lisp_forms(code)
        else:
            forms = split_adder_forms(code)
            
        print(f"Executing {len(forms)} {dialect} forms...")
        responses = run_repl_session(dialect, forms)
        return format_repl_markdown(dialect, forms, responses)
        
    new_content = pattern.sub(replacer, content)
    
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(new_content)
        
    print(f"Book compiled successfully and saved to {output_path}")

    import shutil
    if shutil.which("pandoc"):
        epub_path = output_path.replace(".md", ".epub")
        pdf_path = output_path.replace(".md", ".pdf")
        
        print("Compiling to EPUB...")
        subprocess.run([
            "pandoc", output_path, "-o", epub_path,
            "--metadata", "title=The Alcove & Adder Programming Manual",
            "--metadata", "author=Alcove Team"
        ])
        
        if shutil.which("pdflatex"):
            print("Compiling to PDF...")
            subprocess.run([
                "pandoc", output_path, "-o", pdf_path,
                "--pdf-engine=pdflatex",
                "--metadata", "title=The Alcove & Adder Programming Manual",
                "--metadata", "author=Alcove Team"
            ])

if __name__ == "__main__":
    template = "docs/book_template.md"
    output = "docs/alcove-adder-book.md"
    if len(sys.argv) > 1:
        template = sys.argv[1]
    if len(sys.argv) > 2:
        output = sys.argv[2]
    build_book(template, output)
