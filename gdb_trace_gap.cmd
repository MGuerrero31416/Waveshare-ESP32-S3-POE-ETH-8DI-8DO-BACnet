set logging file gap_trace.txt
set logging on

# Break on GAP advertising start
break esp_ble_gap_start_advertising
commands
  silent
  printf "=== esp_ble_gap_start_advertising hit ===\n"
  python
import time
print("host_time:", time.time())
end
  bt
  continue
end

# Break on GAP config adv data (standard)
break esp_ble_gap_config_adv_data
commands
  silent
  printf "=== esp_ble_gap_config_adv_data hit ===\n"
  python
import time
print("host_time:", time.time())
end
  bt
  continue
end

# Break on GAP config adv data raw (if used)
break esp_ble_gap_config_adv_data_raw
commands
  silent
  printf "=== esp_ble_gap_config_adv_data_raw hit ===\n"
  python
import time
print("host_time:", time.time())
end
  bt
  continue
end

continue
