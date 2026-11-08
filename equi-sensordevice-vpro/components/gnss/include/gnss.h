#pragma once
/*
 * Quectel LC29H GNSS. It is powered whenever the 3.3 V rail is up, so until GPS is
 * needed we put it into backup mode ($PAIR650,0). It then stays there until the rail
 * is power-cycled (i.e. the next battery power-off).
 *
 * Runs in a background task: auto-detects which ESP pin receives NMEA and the baud
 * rate, sends the command, then confirms the NMEA output has stopped.
 */
void gnss_standby_async(void);
