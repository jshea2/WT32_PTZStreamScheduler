# WT32-ETH01 Stream Scheduler for Panasonic PTZ

![Screenshot 2024-07-09 at 6 42 03 PM](https://github.com/user-attachments/assets/322cab7b-d305-4fa1-a72b-e927bb871003)


This uses the WT32-ETH01 to trigger a scheduled start and stop stream command to Panasonic PTZ via HTTP Request.

Features:
- Webpage server for updating configuration
- NTP for getting current time
- Timezone adjustment for offsetting based on location
- Daylight savings time for adding an hour
- IP address feild for PTZ if the address changes
- Saves config even after reboot
- Feedback after updating settings to confirm

Hardware Requirements:
- WT32-ETH01
- USB Flash Kit
- Panasonic PTZ
- Router / Switch

Hardware Recommendations:
- 5V edison adapter to jumper cables  
