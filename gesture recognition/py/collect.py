# collect.py
# Reads gesture data from Nesso N1 over serial and saves to gesture_data.csv
#
# HOW TO USE:
#   1. Flash gesture_collect_v2.ino to the board
#   2. CLOSE Arduino Serial Monitor
#   3. Run: py collect.py
#   4. Hold board still for 3 second calibration
#   5. Select gesture with KEY2, perform gesture
#   6. Repeat until you have 100 captures per gesture
#   7. Press Ctrl+C to stop and save

import serial
import csv
import os
import time

# ─── CONFIG ──────────────────────────────────────────────────────────────────
PORT      = 'COM8'       # Change if your board is on a different port
BAUD      = 115200
CSV_FILE  = 'gesture_data.csv'
# ─────────────────────────────────────────────────────────────────────────────

def main():
    print(f"Opening {PORT} at {BAUD} baud...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=2)
    except serial.SerialException as e:
        print(f"ERROR: Could not open port {PORT}")
        print(f"  {e}")
        print("  Make sure Arduino Serial Monitor is CLOSED and the board is plugged in.")
        return

    time.sleep(2)  # wait for board to boot

    # Open CSV in append mode so data accumulates across sessions
    file_exists = os.path.exists(CSV_FILE) and os.path.getsize(CSV_FILE) > 1
    csvfile = open(CSV_FILE, 'a', newline='')
    writer = csv.writer(csvfile)

    # Write header only if file is new/empty
    if not file_exists:
        writer.writerow(['accelX', 'accelY', 'accelZ', 'gyroX', 'gyroY', 'gyroZ', 'label'])
        print(f"Created new file: {CSV_FILE}")
    else:
        print(f"Appending to existing file: {CSV_FILE}")

    print("Waiting for board...")
    print("Hold the board STILL during the 3-second calibration...")
    print("Press Ctrl+C at any time to stop and save.\n")

    current_label   = None
    current_rows    = []
    total_saved     = 0
    total_discarded = 0

    try:
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()

            if not line:
                continue

            # Board ready after calibration
            if line == 'READY':
                print("Board ready! Select a gesture with KEY2 and start moving.\n")

            # Baseline info
            elif line.startswith('BASELINE:'):
                baseline = line.split(':')[1]
                print(f"Calibrated baseline: {baseline}g")

            # New gesture starting
            elif line.startswith('LABEL:'):
                current_label = line.split(':')[1].strip()
                current_rows  = []
                print(f"  Recording: {current_label}...", end='', flush=True)

            # Good capture — save to CSV
            elif line == 'END':
                if current_label and current_rows:
                    for row in current_rows:
                        writer.writerow(row + [current_label])
                    csvfile.flush()
                    total_saved += 1
                    print(f" saved ({len(current_rows)} samples) | total: {total_saved}")
                current_rows  = []
                current_label = None

            # Bad capture — discard
            elif line == 'DISCARD':
                total_discarded += 1
                print(f" DISCARDED (too weak) | total discarded: {total_discarded}")
                current_rows  = []
                current_label = None

            # Data row
            elif current_label and ',' in line:
                parts = line.split(',')
                if len(parts) == 6:
                    try:
                        row = [float(p) for p in parts]
                        current_rows.append([f"{v:.4f}" for v in row])
                    except ValueError:
                        pass  # skip malformed lines

            # Debug/status lines
            else:
                if line:
                    print(f"  [{line}]")

    except KeyboardInterrupt:
        print(f"\n\nStopped by user.")
        print(f"Total captures saved:    {total_saved}")
        print(f"Total captures discarded: {total_discarded}")
        print(f"Data saved to: {CSV_FILE}")

    finally:
        csvfile.close()
        ser.close()

if __name__ == '__main__':
    main()
