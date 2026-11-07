// Diagnostic log shared by the sign-in tile and the badge service:
// %SystemRoot%\System32\LogFiles\OpenImp\openimp.log. Only SYSTEM and
// Administrators can read or write there: the log names accounts and badge IDs.
// Usernames and events only; never log passwords.

#pragma once

// Appends one timestamped line, printf-style, tagged with the process and thread.
void Log(const wchar_t* format, ...);
