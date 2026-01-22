import os

def generate_txt_files(count=10, output_dir="test_txt"):
    """
    Generate a specified number of text files in the given output directory.
    
    :param count: Number of text files to generate
    :param output_dir: Directory to save the generated text files
    """

    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        print(f"[+] Directory '{output_dir}' created.")
    
    print(f"[+] Generating {count} text files in '{output_dir}'...")

    for i in range(1, count + 1):
        filename = f"{i}.txt"
        filepath = os.path.join(output_dir, filename)
        content = str(i)
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
    
    print(f"[+] Done.")

if __name__ == "__main__":
    generate_txt_files(count=20)