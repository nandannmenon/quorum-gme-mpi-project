import re
import html  # Use the 'html' module

# --- Configuration ---
LOG_FILE = "output.log"
HTML_FILE = "visualization.html"

# --- Define CSS Styles and Color Palette ---
# We'll assign colors to processes as we find them
process_colors = {}
color_palette = [
    # Manager Colors (cyans/blues)
    "#00BCD4", "#03A9F4", "#3F51B5",
    # Requester Group 0 Colors (greens)
    "#4CAF50", "#8BC34A", "#CDDC39",
    # Requester Group 1 Colors (oranges/yellows)
    "#FFC107", "#FF9800", "#FF5722",
    # Other processes
    "#9E9E9E", "#795548", "#607D8B",
]

def get_process_color(process_name):
    """Assigns a consistent color to each process."""
    if process_name not in process_colors:
        # Try to match group for better colors
        color = None
        if "Manager" in process_name:
            # Find a manager color
            color = next((c for c in color_palette if c.startswith("#0") or c.startswith("#3")), None)
        elif "group is 0" in process_name:
            color = next((c for c in color_palette if c.startswith("#4") or c.startswith("#8")), None)
        elif "group is 1" in process_name:
            color = next((c for c in color_palette if c.startswith("#F")), None)
        
        # Simple fallback if smart selection fails
        if color is None:
             color = color_palette.pop(0) if color_palette else "#FFFFFF"
             
        # Store the chosen color
        if "Manager" in process_name:
            process_colors[process_name] = color
        elif "Requester" in process_name:
            # Extract "Requester X" from the longer string
            key_match = re.search(r"(Requester \d+)", process_name)
            if key_match:
                key = key_match.group(1)
                process_colors[key] = color
            
    # Retrieve the color
    key = process_name
    if "Requester" in process_name:
        key_match = re.search(r"(Requester \d+)", process_name)
        if key_match:
            key = key_match.group(1)
            
    return process_colors.get(key, "#EEEEEE") # Default to gray


# --- HTML Templates ---
HTML_HEADER = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>MPI Log Visualization</title>
    <style>
        body { background-color: #1e1e1e; color: #d4d4d4; font-family: 'SFMono-Regular', Consolas, 'Courier New', monospace; line-height: 1.6; }
        .container { width: 90%; margin: 20px auto; padding: 20px; background-color: #252526; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
        h1 { color: #4fc1ff; border-bottom: 2px solid #4fc1ff; padding-bottom: 10px; }
        .log-line { white-space: pre-wrap; word-break: break-all; }
        .log-line span { display: inline-block; padding: 2px 6px; border-radius: 3px; margin-right: 10px; font-weight: bold; }
        .cs-enter { background-color: #28a745; color: #ffffff; font-weight: bold; padding: 5px 0; display: block; text-align: center; }
        .cs-exit { background-color: #dc3545; color: #ffffff; font-weight: bold; padding: 5px 0; display: block; text-align: center; }
    </style>
</head>
<body>
    <div class="container">
        <h1>MPI Process Log</h1>
        <div class="log-content">
"""

HTML_FOOTER = """
        </div>
    </div>
</body>
</html>
"""

# --- Main Script ---
try:
    with open(LOG_FILE, "r") as f_in, open(HTML_FILE, "w") as f_out:
        f_out.write(HTML_HEADER)

        for line in f_in:
            line = line.strip()
            if not line:
                continue

            # Escape HTML characters like < and >
            safe_line = html.escape(line) # <-- Use html.escape

            # Check for special CS lines
            if "ENTERING CS" in line:
                f_out.write(f'<div class="log-line cs-enter">{safe_line}</div>\n')
                continue
            if "EXITING CS" in line:
                f_out.write(f'<div class="log-line cs-exit">{safe_line}</div>\n')
                continue

            # Try to find a process name
            match = re.search(r"\[(.*?)\]:", line)
            if match:
                process_name = match.group(1)
                # Handle the "Online" line which has extra info
                if "Online" in line and "Requester" in process_name:
                     process_name = line[1:line.find("]:")+1] # "Requester X]: Online. My group is Y"
                
                color = get_process_color(process_name)
                
                # Re-build the line for HTML
                tag = f'<span style="background-color: {color}; color: #000000;">{html.escape(process_name)}</span>' # <-- Use html.escape
                message = html.escape(line[match.end(0):]) # <-- Use html.escape
                f_out.write(f'<div class="log-line">{tag}{message}</div>\n')
            else:
                # Line without a process tag (like a blank line)
                f_out.write(f'<div class="log-line">{safe_line}</div>\n')

        f_out.write(HTML_FOOTER)
        
    print(f"Success! Visualization saved to {HTML_FILE}")

except FileNotFoundError:
    print(f"Error: Log file '{LOG_FILE}' not found.")
    print(f"Please run 'mpirun -np 6 ./gme_mpi > {LOG_FILE} 2>&1' first.")
except Exception as e:
    print(f"An error occurred: {e}")w