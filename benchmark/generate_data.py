import json

import loremipsum


def generate(line_length, total_length):
    lines = []
    current_length = 0
    while True:
        current_line_length = 0
        line = []
        while True:
            s = loremipsum.get_sentence()
            line.append(s)
            current_line_length += len(s)
            if current_line_length > line_length:
                break

        lines.append(' '.join(line) + '\n')
        current_length += len(lines[-1])
        if current_length > total_length:
            break

    return ''.join(lines)


data = [
    {
        'lines': line_label,
        'length': length_label,
        'data': generate(line_length, total_length),
    }
    for line_label, line_length in [
        ('short_lines', 1),
        ('128b_lines', 128),
        ('1k_lines', 1024),
        ('10k_lines', 1024 * 10),
        ('1m_lines', 1024 * 1024),
    ]
    for length_label, total_length in [
        ('1k', 1024),
        ('10k', 1024 * 10),
        ('1m', 1024 * 1024),
        ('10m', 1024 * 1024 * 10),
    ]
    if line_length < total_length
]
with open('data.json', 'w') as outfile:
    json.dump(data, outfile)
