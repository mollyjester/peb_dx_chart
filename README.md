# peb_dx_chart

A Pebble watch app that displays a continuous glucose monitoring (CGM) chart from Dexcom Share data.

## Features

- **3-Hour Glucose Chart**: Displays blood glucose readings for the last 3 hours
- **Timeline Layout**: Most recent reading at bottom, older readings going up (timeline goes up the y-axis)
- **Value Display**: Glucose values displayed horizontally along the x-axis
- **Threshold Lines**: Shows safe range boundaries (70-180 mg/dL or 4-10 mmol/L)
- **Auto-Refresh**: Automatically fetches new data every 5 minutes
- **Configurable Settings**: Set Dexcom credentials and choose units (mg/dL or mmol/L)

## Installation

1. Install the Pebble SDK if you haven't already
2. Clone this repository
3. Run `pebble build` to compile the app
4. Run `pebble install --phone <phone_ip>` to install on your watch

## Configuration

After installing the app on your Pebble watch:

1. Open the Pebble app on your phone
2. Navigate to the "Dexcom Chart" app settings
3. Enter your Dexcom Share credentials:
   - **Login**: Your Dexcom Share username/email
   - **Password**: Your Dexcom Share password
   - **Region**: Select your Dexcom server region (US, Outside US, or Japan)
4. Choose your preferred **Blood Glucose Units** (mg/dL or mmol/L)
5. Save the settings

The app will automatically fetch your glucose data and display it on the chart.

## Chart Layout

The chart displays:
- **Y-axis (vertical)**: Time, with most recent at bottom going up to oldest at top (each line represents 5 minutes)
- **X-axis (horizontal)**: Blood glucose values
- **Red vertical lines**: Low (70 mg/dL / 4 mmol/L) and high (180 mg/dL / 10 mmol/L) thresholds
- **White line with dots**: Your glucose readings connected chronologically from bottom (newest) to top (oldest)
- **Grid lines**: Help read values (every 50 mg/dL / 3 mmol/L horizontally, every 30 minutes vertically)
- **Time labels**: Show how many minutes ago each reading was taken (-0m at bottom, -30m, -60m, etc. going up)

## Requirements

- Pebble smartwatch (any model compatible with Pebble SDK 3)
- Active Dexcom CGM system
- Dexcom Share account with data sharing enabled
- Pebble/Rebble app on your phone
- Internet connection on phone

## Based on

This app uses the Dexcom integration code from [rat_scout](https://github.com/mollyjester/rat_scout), a comprehensive Pebble watchface with CGM support.

## Credits

- Dexcom API integration based on [pydexcom](https://github.com/gagebenne/pydexcom)
- Built for Pebble Core 2 Duo watches

## License

This project is provided as-is for personal use.

