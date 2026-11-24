"""Embed the receiver's OpenAPI contract and render its offline HTML reference."""
import json
import sys
from html import escape
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = json.loads((root / 'docs/api/openapi.json').read_text(encoding='utf-8'))
groups = {}
for path, methods in spec['paths'].items():
    for method, op in methods.items():
        example = path
        parameters = []
        for param in op.get('parameters', []):
            name = param['name']
            example = example.replace('{' + name + '}', str(param['example']))
            description = param['description']
            if 'enum' in param['schema']:
                description += ' ' + ', '.join(param['schema']['enum'])
            parameters.append(f'<dt>{escape(name)}</dt><dd>{escape(description)}</dd>')
        codes = []
        for code, response in op['responses'].items():
            if '$ref' in response:
                response = spec['components']['responses'][response['$ref'].split('/')[-1]]
            codes.append(f'<dt>{escape(code)}</dt><dd>{escape(response["description"])}</dd>')
        method = method.upper()
        sample = escape(f'{method} {example}')
        example_html = f'<pre>{sample}</pre>' if method == 'PUT' else f'<p><a href="{escape(path)}">Open {escape(path)}</a></p>'
        details = f'<dl>{"".join(parameters)}</dl>' if parameters else ''
        section = f'''<details><summary><b class="{method}">{method}</b>
<code>{escape(path)}</code><span>{escape(op['summary'])}</span></summary>
<div><p>{escape(op['description'])}</p>{details}{example_html}
<h3>Antwoorden</h3><dl>{"".join(codes)}</dl></div></details>'''
        groups.setdefault(op['tags'][0], []).append(section)

sections = '\n'.join(f'<section><h2>{escape(tag)}</h2>{"".join(groups[tag])}</section>'
                     for tag in ('Uitlezen', 'Bedienen', 'Documentatie'))
page = (root / 'docs/api/reference.html').read_text(encoding='utf-8')
page = page.replace('{{title}}', escape(spec['info']['title'])).replace(
    '{{intro}}', escape(spec['info']['description'])).replace('{{sections}}', sections)

def c_string(value):
    return '\n'.join(json.dumps(line, ensure_ascii=False) for line in value.splitlines(keepends=True))

header = (f'static const char api_reference_html[] =\n{c_string(page)};\n'
          f'static const char api_openapi_json[] =\n{c_string(json.dumps(spec, indent=2, ensure_ascii=False))};\n')
target = Path(sys.argv[1])
target.parent.mkdir(parents=True, exist_ok=True)
if not target.exists() or target.read_text(encoding='utf-8') != header:
    target.write_text(header, encoding='utf-8')
