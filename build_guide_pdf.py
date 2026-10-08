import re
import html
import subprocess
import os
import sys

def md_to_html(md_text):
    lines = md_text.split('\n')
    out = []
    in_code = False
    in_table = False
    in_ul = False
    in_ol = False
    
    for line in lines:
        if line.startswith('```'):
            if in_code:
                out.append('</code></pre>')
                in_code = False
            else:
                lang = line[3:].strip()
                out.append(f'<pre><code class="language-{lang}">')
                in_code = True
            continue
        if in_code:
            out.append(html.escape(line))
            continue
            
        # Tables
        if line.startswith('|') and line.endswith('|'):
            if not in_table:
                out.append('<div class="table-wrap"><table>')
                in_table = True
                is_header = True
            else:
                is_header = False
                
            if re.match(r'^\|[\s\-:|]+\|$', line.strip()):
                continue # delimiter row
                
            cols = [c.strip() for c in line.strip().split('|')[1:-1]]
            tag = 'th' if is_header else 'td'
            row_html = ''.join(f'<{tag}>{inline_format(c)}</{tag}>' for c in cols)
            out.append(f'<tr>{row_html}</tr>')
            continue
        elif in_table:
            out.append('</table></div>')
            in_table = False
            
        # Headings
        if line.startswith('# '):
            out.append(f'<h1>{inline_format(line[2:])}</h1>')
        elif line.startswith('## '):
            out.append(f'<h2>{inline_format(line[3:])}</h2>')
        elif line.startswith('### '):
            out.append(f'<h3>{inline_format(line[4:])}</h3>')
        elif line.startswith('#### '):
            out.append(f'<h4>{inline_format(line[5:])}</h4>')
        elif line.startswith('- '):
            if not in_ul:
                out.append('<ul>')
                in_ul = True
            out.append(f'<li>{inline_format(line[2:])}</li>')
        elif re.match(r'^[0-9]+\.\s', line):
            if not in_ol:
                out.append('<ol>')
                in_ol = True
            item = re.sub(r'^[0-9]+\.\s', '', line)
            out.append(f'<li>{inline_format(item)}</li>')
        elif line.strip() == '---':
            if in_ul: out.append('</ul>'); in_ul = False
            if in_ol: out.append('</ol>'); in_ol = False
            out.append('<hr/>')
        elif line.strip() == '':
            if in_ul: out.append('</ul>'); in_ul = False
            if in_ol: out.append('</ol>'); in_ol = False
        else:
            if in_ul: out.append('</ul>'); in_ul = False
            if in_ol: out.append('</ol>'); in_ol = False
            out.append(f'<p>{inline_format(line)}</p>')
            
    if in_code: out.append('</code></pre>')
    if in_table: out.append('</table></div>')
    if in_ul: out.append('</ul>')
    if in_ol: out.append('</ol>')
    return '\n'.join(out)

def inline_format(text):
    # Escape basic HTML chars except what we want
    text = html.escape(text)
    # Bold: **text**
    text = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', text)
    # Italic: *text*
    text = re.sub(r'\*(.+?)\*', r'<em>\1</em>', text)
    # Inline code: `text`
    text = re.sub(r'`(.+?)`', r'<code>\1</code>', text)
    # Math: $...$
    text = re.sub(r'\$(.+?)\$', r'<span class="math">\1</span>', text)
    # Links: [text](url)
    text = re.sub(r'\[(.+?)\]\((.+?)\)', r'<a href="\2">\1</a>', text)
    # Tags: [DATA REQUIRED: ...]
    text = re.sub(r'\[DATA REQUIRED:\s*(.+?)\]', r'<span class="badge-required">[DATA REQUIRED: \1]</span>', text)
    # Tags: [CITATION REQUIRED: ...]
    text = re.sub(r'\[CITATION REQUIRED:\s*(.+?)\]', r'<span class="badge-citation">[CITATION REQUIRED: \1]</span>', text)
    return text

def main():
    root = os.path.dirname(os.path.abspath(__file__))
    md_file = os.path.join(root, 'FORMAL_REPORT_GUIDE.md')
    html_file = os.path.join(root, 'FORMAL_REPORT_GUIDE.html')
    pdf_file = os.path.join(root, 'FORMAL_REPORT_GUIDE.pdf')
    
    with open(md_file, 'r', encoding='utf-8') as f:
        md_text = f.read()
        
    body_html = md_to_html(md_text)
    
    full_html = f'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>FORMAL REPORT GUIDE - DeadlockScope</title>
<style>
  @page {{
    size: A4;
    margin: 18mm 16mm 18mm 16mm;
    @bottom-right {{
      content: counter(page);
    }}
  }}
  :root {{
    --primary: #0f172a;
    --accent: #1d4ed8;
    --text: #1e293b;
    --border: #cbd5e1;
    --bg-light: #f8fafc;
    --crimson: #dc2626;
    --amber: #d97706;
    --emerald: #059669;
  }}
  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    font-family: "Segoe UI", -apple-system, BlinkMacSystemFont, Roboto, Helvetica, Arial, sans-serif;
    font-size: 10pt;
    line-height: 1.55;
    color: var(--text);
    background: #fff;
    padding: 24px 32px;
    max-width: 960px;
    margin: 0 auto;
  }}
  h1 {{
    font-size: 20pt;
    font-weight: 800;
    color: var(--primary);
    border-bottom: 3px solid var(--primary);
    padding-bottom: 8px;
    margin-top: 24px;
    margin-bottom: 16px;
    page-break-after: avoid;
  }}
  h2 {{
    font-size: 14pt;
    font-weight: 700;
    color: var(--accent);
    border-bottom: 1.5px solid var(--border);
    padding-bottom: 5px;
    margin-top: 22px;
    margin-bottom: 12px;
    page-break-after: avoid;
  }}
  h3 {{
    font-size: 11.5pt;
    font-weight: 600;
    color: var(--primary);
    margin-top: 16px;
    margin-bottom: 8px;
    page-break-after: avoid;
  }}
  h4 {{
    font-size: 10.5pt;
    font-weight: 600;
    color: #334155;
    margin-top: 12px;
    margin-bottom: 6px;
  }}
  p {{
    margin-bottom: 10px;
    text-align: justify;
  }}
  ul, ol {{
    margin-left: 24px;
    margin-bottom: 12px;
  }}
  li {{
    margin-bottom: 4px;
  }}
  hr {{
    border: none;
    border-top: 1px solid var(--border);
    margin: 20px 0;
  }}
  .table-wrap {{
    width: 100%;
    overflow-x: auto;
    margin: 14px 0;
    page-break-inside: avoid;
  }}
  table {{
    width: 100%;
    border-collapse: collapse;
    font-size: 8.5pt;
    margin-bottom: 8px;
  }}
  th, td {{
    border: 1px solid var(--border);
    padding: 6px 10px;
    text-align: left;
    vertical-align: top;
  }}
  th {{
    background-color: var(--bg-light);
    color: var(--primary);
    font-weight: 700;
  }}
  tr:nth-child(even) {{
    background-color: #fbfcfe;
  }}
  pre {{
    background: #0f172a;
    color: #e2e8f0;
    padding: 12px 14px;
    border-radius: 6px;
    font-family: "Consolas", "Courier New", monospace;
    font-size: 8.5pt;
    line-height: 1.45;
    overflow-x: auto;
    margin: 12px 0;
    page-break-inside: avoid;
  }}
  code {{
    font-family: "Consolas", "Courier New", monospace;
    font-size: 9pt;
    background: #f1f5f9;
    color: #b91c1c;
    padding: 2px 4px;
    border-radius: 3px;
  }}
  pre code {{
    background: transparent;
    color: inherit;
    padding: 0;
  }}
  .math {{
    font-family: "Cambria Math", "Times New Roman", serif;
    font-style: italic;
  }}
  .badge-required {{
    display: inline-block;
    background: #fef2f2;
    color: var(--crimson);
    border: 1px solid #fecaca;
    font-size: 8pt;
    font-weight: 600;
    padding: 1px 6px;
    border-radius: 4px;
    margin: 0 2px;
  }}
  .badge-citation {{
    display: inline-block;
    background: #eff6ff;
    color: var(--accent);
    border: 1px solid #bfdbfe;
    font-size: 8pt;
    font-weight: 600;
    padding: 1px 6px;
    border-radius: 4px;
    margin: 0 2px;
  }}
</style>
</head>
<body>
{body_html}
</body>
</html>
'''
    
    with open(html_file, 'w', encoding='utf-8') as f:
        f.write(full_html)
    print(f"Generated HTML: {html_file}")
    
    edge_path = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
    if os.path.exists(edge_path):
        cmd = [
            edge_path,
            "--headless=new",
            "--disable-gpu",
            "--no-pdf-header-footer",
            f"--print-to-pdf={pdf_file}",
            html_file
        ]
        print("Invoking Edge headless to generate PDF...")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            print(f"Successfully compiled PDF: {pdf_file} ({os.path.getsize(pdf_file)} bytes)")
        else:
            print(f"Edge error: {res.stderr}")
    else:
        print(f"Edge not found at {edge_path}")

if __name__ == '__main__':
    main()
