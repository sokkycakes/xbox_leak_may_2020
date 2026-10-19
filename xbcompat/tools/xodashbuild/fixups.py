#!/usr/bin/env python3
"""Source fixes for the leak's XOnlineDash, applied to patched copies by
build.sh:  fixups.py FILE < source > patched

The account sign-up in this snapshot starts _XOnlineCreateAccount and shows
"Congrats!" straight away: nothing pumps the task, so the request never goes
out until some later scene happens to, and its result is thrown away.  These
run the wait cursor until the task ends and report a failure instead."""
import sys

FIXES = {
    'act_online.cpp': [(
        '                        hr = Accounts::Instance()->Validate( eACT_Cost_Confirm_Yes, L"" );\r\n'
        '                        hDialogReturn = RunOKDlg( L"SIGNUP_CONGRATS_MESSAGE" );\r\n',
        '                        hr = Accounts::Instance()->Validate( eACT_Cost_Confirm_Yes, L"" );\r\n'
        '                        while ( hr == XBMSG_NEED_WAITCURSOR )\r\n'
        '                            hr = RunWaitCursor( NULL, Accounts::UpdateAccountInfo, _T("CANCEL") );\r\n'
        '                        if ( FAILED( hr ) && hr != XBMSG_OK )\r\n'
        '                            hDialogReturn = RunOKDlg( _T("ERR_NO_SERVER") );\r\n'
        '                        else\r\n'
        '                            hDialogReturn = RunOKDlg( L"SIGNUP_CONGRATS_MESSAGE" );\r\n',
    )],
    'accountdata.cpp': [(
        '            hr = EndTask( m_nCurTask );\r\n'
        '            // do something here\r\n'
        '            hr = XBMSG_OK;\r\n',
        '            hr = EndTask( m_nCurTask );\r\n'
        '            if ( SUCCEEDED( hr ) )\r\n'
        '                hr = XBMSG_OK;\r\n',
    )],
}

name = sys.argv[1].lower()
src = sys.stdin.buffer.read().decode('latin-1')
for old, new in FIXES.get(name, []):
    if src.count(old) != 1:
        sys.exit(f'fixups.py: {name}: expected text not found once')
    src = src.replace(old, new)
sys.stdout.buffer.write(src.encode('latin-1'))
