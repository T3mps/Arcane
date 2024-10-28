import sys
import os
from string import Template

def read_template(template_path):
    with open(template_path, 'r') as f:
        return Template(f.read())

def write_rc(output_path, content):
    with open(output_path, 'w') as f:
        f.write(content)

def main():
    if len(sys.argv) < 3:
        print("Usage: generate_module_rc.py <template_path> <output_path> [version] [company] [description]")
        sys.exit(1)

    template_path = sys.argv[1]
    output_path = sys.argv[2]
    
    # Default values
    substitutions = {
    'MODULE_VERSION_MAJOR': '1',
    'MODULE_VERSION_MINOR': '0',
    'MODULE_VERSION_PATCH': '0',
    'MODULE_VERSION_STRING': '1.0.0.0',
    'MODULE_COMPANY': 'Arcane Engine',
    'MODULE_DESCRIPTION': os.path.splitext(os.path.basename(output_path))[0],
    'MODULE_NAME': os.path.splitext(os.path.basename(output_path))[0],
    'MODULE_COPYRIGHT': '(C) 2024 Arcane Engine',
    'MODULE_FILENAME': os.path.basename(output_path),
    'MODULE_BUILD_CONFIG': 'Debug' if '_debug' in output_path.lower() else 'Release',
    # New ENGINE version placeholders
    'ENGINE_VERSION_MAJOR': '1',
    'ENGINE_VERSION_MINOR': '0',
    'ENGINE_VERSION_PATCH': '0',
    'ENGINE_VERSION_STRING': '1.0.0'
}

    if len(sys.argv) > 3:
        version_parts = sys.argv[3].split('.')
        if len(version_parts) >= 3:
            substitutions['MODULE_VERSION_MAJOR'] = version_parts[0]
            substitutions['MODULE_VERSION_MINOR'] = version_parts[1]
            substitutions['MODULE_VERSION_PATCH'] = version_parts[2]
        substitutions['MODULE_VERSION_STRING'] = sys.argv[3]

    if len(sys.argv) > 4:
        substitutions['MODULE_COMPANY'] = sys.argv[4]
    if len(sys.argv) > 5:
        substitutions['MODULE_DESCRIPTION'] = sys.argv[5]

    template = read_template(template_path)
    content = template.substitute(substitutions)
    write_rc(output_path, content)

if __name__ == "__main__":
    main()