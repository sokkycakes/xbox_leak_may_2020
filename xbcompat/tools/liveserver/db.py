"""The Live server's state: its own keys, consoles, machine accounts and users,
kept in one JSON file."""
import json
import os
import threading

import kdc


class Db:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        try:
            with open(path) as f:
                self.d = json.load(f)
        except FileNotFoundError:
            self.d = {}
        self.d.setdefault('keys', {})
        self.d.setdefault('consoles', {kdc.DEVKIT_SERIAL: kdc.DEVKIT_ONLINE_KEY.hex()})
        self.d.setdefault('machines', {})
        self.d.setdefault('users', {})
        self.d.setdefault('next_xuid', 0x0009000000000001)
        self.save()

    def save(self):
        tmp = self.path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.d, f, indent=1)
        os.replace(tmp, self.path)

    def _key(self, name):
        with self.lock:
            if name not in self.d['keys']:
                self.d['keys'][name] = os.urandom(16).hex()
                self.save()
            return bytes.fromhex(self.d['keys'][name])

    def krbtgt_key(self):
        return self._key('krbtgt')

    def sg_key(self):
        return self._key('sg')

    def online_key(self, serial):
        k = self.d['consoles'].get(serial)
        return bytes.fromhex(k) if k else None

    def _xuid(self):
        x = self.d['next_xuid']
        self.d['next_xuid'] = x + 1
        return x

    def new_machine(self, serial):
        with self.lock:
            x = self._xuid()
            acct = {'xuid': x, 'name': 'SN%012X' % (x & 0xFFFFFFFFFFFF), 'domain': kdc.MACHINE_REALM,
                    'realm': kdc.MACHINE_REALM, 'key': os.urandom(16).hex(), 'serial': serial,
                    'machine': True}
            self.d['machines'][acct['name'] + '@' + acct['domain']] = acct
            self.save()
            return acct

    def new_user(self, nick, kingdom, domain, realm):
        with self.lock:
            x = self._xuid()
            acct = {'xuid': x, 'nick': nick, 'kingdom': kingdom, 'domain': domain, 'realm': realm,
                    'key': os.urandom(16).hex()}
            self.d['users'][self.user_principal(nick, kingdom, domain)] = acct
            self.save()
            return acct

    @staticmethod
    def user_principal(nick, kingdom, domain):
        return nick + ('.' + kingdom if kingdom else '') + ('@' + domain if domain else '')

    def find_principal(self, cname, realm):
        for table in ('machines', 'users'):
            for k, v in self.d[table].items():
                if k.lower() == cname.lower() and v['realm'].lower() == realm.lower():
                    return v
        return None

    def name_taken(self, nick, kingdom):
        return any(u['nick'].lower() == nick.lower() and u['kingdom'].lower() == kingdom.lower()
                   for u in self.d['users'].values())
