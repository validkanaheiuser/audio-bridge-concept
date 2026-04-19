import sys

def check_brace(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    balance = 0
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    escape = False
    
    stack = []
    
    i = 0
    while i < len(content):
        c = content[i]
        
        if in_line_comment:
            if c == '\n':
                in_line_comment = False
        elif in_block_comment:
            if c == '*' and i + 1 < len(content) and content[i+1] == '/':
                in_block_comment = False
                i += 1
        elif in_string:
            if escape:
                escape = False
            elif c == '\\':
                escape = True
            elif c == '"':
                in_string = False
        elif in_char:
            if escape:
                escape = False
            elif c == '\\':
                escape = True
            elif c == "'":
                in_char = False
        else:
            if c == '/' and i + 1 < len(content) and content[i+1] == '/':
                in_line_comment = True
                i += 1
            elif c == '/' and i + 1 < len(content) and content[i+1] == '*':
                in_block_comment = True
                i += 1
            elif c == '"':
                in_string = True
            elif c == "'":
                in_char = True
            elif c == '{':
                balance += 1
                line_no = content[:i].count('\n') + 1
                stack.append(line_no)
            elif c == '}':
                balance -= 1
                if balance < 0:
                    line_no = content[:i].count('\n') + 1
                    print(f"Extraneous }} at line {line_no}")
                    return
                stack.pop()
        
        i += 1
        
    if balance > 0:
        print(f"Unclosed braces opened at lines: {stack}")
    else:
        print("PERFECT BALANCE")

check_brace('jni/audio_bridge.cpp')
