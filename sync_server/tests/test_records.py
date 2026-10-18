"""Records: optimistic versions, conflict 409s, replay keys, tombstones, cursor."""
import concurrent.futures
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import ServerCase


class RecordTests(ServerCase):
    def setUp(self):
        super().setUp()
        self.jar, _ = self.user()
        self.device = self.token_for(self.jar)['token']
        self.vid = self.vault(self.jar, 'Records')['id']

    def test_create_returns_the_record_and_round_trips_every_field(self):
        package = 'a' * 64
        status, _, value = self.put_record(self.device, self.vid, 'r1', 0, name='Report.pdf',
                                           path='Work/2026', tags=['finance', 'pdf'],
                                           package=package, size=1024)
        self.assertEqual(status, 201, value)
        record = value['record']
        self.assertEqual(record['id'], 'r1')
        self.assertEqual((record['version'], record['deleted']), (1, False))
        self.assertEqual((record['name'], record['path'], record['tags'], record['package'], record['size']),
                         ('Report.pdf', 'Work/2026', ['finance', 'pdf'], package, 1024))
        self.assertGreater(record['updated'], 0)
        status, _, value = self.request('GET', '/v1/vaults/%s/records/r1' % self.vid, token=self.device)
        self.assertEqual((status, value['record']), (200, record))

    def test_bad_records_are_refused_before_they_are_stored(self):
        cases = [
            {'name': ''}, {'name': 'x' * 256}, {'path': 'x' * 1025}, {'tags': 'not-a-list'},
            {'tags': ['ok', '']}, {'tags': ['x' * 65]}, {'package': 'not-a-hash'},
            {'package': 'A' * 64}, {'size': -1}, {'size': 'big'}, {'size': 1024**3 + 1},
        ]
        for fields in cases:
            status, _, value = self.put_record(self.device, self.vid, 'bad', 0, **fields)
            self.assertEqual(status, 400, (fields, value))
        self.assertEqual(self.records(self.device, self.vid)['records'], [])
        self.assertEqual(self.put_record(self.device, self.vid, 'bad', -1)[0], 400)
        self.assertEqual(self.put_record(self.device, self.vid, 'bad', True)[0], 400)
        body = {'base_version': 0, 'name': 'n'}
        self.assertEqual(self.request('PUT', '/v1/vaults/%s/records/n' % self.vid, body,
                                      token=self.device)[0], 400, 'operation id is required')
        self.assertEqual(self.request('PUT', '/v1/vaults/%s/records/../../etc' % self.vid, body,
                                      token=self.device)[0], 404)
        self.assertEqual(self.records(self.device, self.vid)['records'], [])

    def test_update_bumps_the_version_and_a_stale_base_is_a_409_with_the_current_row(self):
        self.assertEqual(self.put_record(self.device, self.vid, 'r1', 0, name='notes.txt')[0], 201)
        status, _, value = self.put_record(self.device, self.vid, 'r1', 1, name='notes v2.txt',
                                           path='Inbox', tags=['edited'], size=9)
        self.assertEqual(status, 200, value)
        self.assertEqual((value['record']['version'], value['record']['name']), (2, 'notes v2.txt'))
        # device B still believes it is version 1
        status, _, value = self.put_record(self.device, self.vid, 'r1', 1, name='notes from B.txt')
        self.assertEqual(status, 409, value)
        self.assertEqual(value['error'], 'version conflict')
        self.assertEqual(value['record']['version'], 2)
        self.assertEqual(value['record']['name'], 'notes v2.txt', 'the 409 must carry the current record')
        self.assertEqual(self.records(self.device, self.vid)['records'][0]['name'], 'notes v2.txt')
        # what the client does with it: a new record id, both survive
        status, _, conflict = self.put_record(self.device, self.vid, 'r1-conflict', 0,
                                              name='notes from B (conflict copy).txt')
        self.assertEqual(status, 201)
        names = {row['name'] for row in self.records(self.device, self.vid)['records']}
        self.assertEqual(names, {'notes v2.txt', 'notes from B (conflict copy).txt'})
        self.assertEqual(conflict['record']['version'], 1)

    def test_updating_a_record_that_is_not_there_conflicts_rather_than_resurrecting_it(self):
        status, _, value = self.put_record(self.device, self.vid, 'ghost', 3, name='ghost.txt')
        self.assertEqual((status, value), (409, {'error': 'version conflict', 'record': None}))

    def test_a_repeated_operation_replays_the_stored_answer(self):
        operation = 'op-replay-0001'
        first = self.put_record(self.device, self.vid, 'r1', 0, operation=operation, name='once.txt')
        self.assertEqual(first[0], 201)
        second = self.put_record(self.device, self.vid, 'r1', 0, operation=operation, name='twice.txt')
        self.assertEqual(second[0], 201, 'a replay answers with the original status')
        self.assertEqual(second[2], first[2], 'a replay answers with the original body')
        record = self.records(self.device, self.vid)['records'][0]
        self.assertEqual((record['name'], record['version']), ('once.txt', 1), 'the replay changed the record')
        # the replay must not be mistaken for a stale write either
        later = self.put_record(self.device, self.vid, 'r1', 1, name='after.txt')
        self.assertEqual((later[0], later[2]['record']['version']), (200, 2))
        # and a different operation id on the same record really does apply
        other = self.put_record(self.device, self.vid, 'r1', 1, operation='op-replay-0002', name='retry.txt')
        self.assertEqual(other[0], 409, 'base 1 is now stale; the replay key does not bypass the check')

    def test_cursor_walks_only_what_changed(self):
        empty = self.records(self.device, self.vid)
        self.assertEqual((empty['records'], empty['cursor']), ([], 0))
        self.put_record(self.device, self.vid, 'r1', 0, name='one')
        self.put_record(self.device, self.vid, 'r2', 0, name='two')
        first = self.records(self.device, self.vid)
        self.assertEqual([row['id'] for row in first['records']], ['r1', 'r2'])
        self.assertEqual(first['cursor'], 2)
        self.put_record(self.device, self.vid, 'r1', 1, name='one renamed')
        delta = self.records(self.device, self.vid, since=first['cursor'])
        self.assertEqual([(row['id'], row['version'], row['name']) for row in delta['records']],
                         [('r1', 2, 'one renamed')])
        self.assertEqual(delta['cursor'], 3)
        self.assertEqual(self.records(self.device, self.vid, since=delta['cursor'])['records'], [])
        # paging: a short page stops at the last row it returned, so the client
        # can ask again from there without skipping anything
        fresh = self.vault(self.jar, 'Paging')['id']
        self.put_record(self.device, fresh, 'a', 0, name='a')
        self.put_record(self.device, fresh, 'b', 0, name='b')
        page = self.records(self.device, fresh, limit=1)
        self.assertEqual([row['id'] for row in page['records']], ['a'])
        self.assertEqual(page['cursor'], 1)
        rest = self.records(self.device, fresh, since=page['cursor'])
        self.assertEqual([row['id'] for row in rest['records']], ['b'])
        self.assertEqual(rest['cursor'], 2)

    def test_tombstone_restore_and_purge(self):
        self.put_record(self.device, self.vid, 'r1', 0, name='keep me')
        cursor = self.records(self.device, self.vid)['cursor']
        status, _, value = self.request('DELETE', '/v1/vaults/%s/records/r1' % self.vid, token=self.device)
        self.assertEqual(status, 200, value)
        self.assertEqual((value['record']['deleted'], value['record']['version']), (True, 2))
        listed = self.records(self.device, self.vid, since=cursor)
        self.assertEqual([(row['id'], row['deleted']) for row in listed['records']], [('r1', True)],
                         'a delete is visible to other devices as a tombstone')
        # a device that had not seen the delete cannot clobber it with a stale write
        self.assertEqual(self.put_record(self.device, self.vid, 'r1', 1, name='back')[0], 409)
        status, _, value = self.request('POST', '/v1/vaults/%s/records/r1/restore' % self.vid, token=self.device)
        self.assertEqual((status, value['record']['deleted'], value['record']['version']), (200, False, 3))
        again = self.request('POST', '/v1/vaults/%s/records/r1/restore' % self.vid, token=self.device)
        self.assertEqual(again[2]['record']['version'], 3, 'restoring a live record changes nothing')
        self.assertEqual(self.request('DELETE', '/v1/vaults/%s/records/r1' % self.vid, token=self.device)[0], 200)
        status, _, _ = self.request('DELETE', '/v1/vaults/%s/records/r1?purge=1' % self.vid, token=self.device)
        self.assertEqual(status, 204)
        self.assertEqual(self.request('GET', '/v1/vaults/%s/records/r1' % self.vid, token=self.device)[0], 404)
        self.assertEqual(self.records(self.device, self.vid)['records'], [])
        self.assertEqual(self.request('DELETE', '/v1/vaults/%s/records/r1?purge=1' % self.vid,
                                      token=self.device)[0], 404, 'purging twice is a 404')
        self.assertEqual(self.request('POST', '/v1/vaults/%s/records/r1/restore' % self.vid,
                                      token=self.device)[0], 404)

    def test_two_devices_writing_the_same_base_leave_one_winner(self):
        self.put_record(self.device, self.vid, 'r1', 0, name='start')

        def write(index):
            return self.put_record(self.device, self.vid, 'r1', 1, name='writer %d' % index)[0]

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            statuses = sorted(pool.map(write, range(4)))
        self.assertEqual(statuses, [200, 409, 409, 409], 'exactly one writer may win a version')
        winner = self.records(self.device, self.vid)['records'][0]
        self.assertEqual(winner['version'], 2)

    def test_records_and_blobs_are_scoped_to_their_vault(self):
        other = self.vault(self.jar, 'Elsewhere')['id']
        self.put_record(self.device, self.vid, 'r1', 0, name='here')
        self.assertEqual(self.records(self.device, other)['records'], [])
        self.assertEqual(self.request('GET', '/v1/vaults/%s/records/r1' % other, token=self.device)[0], 404)
        status, _, _ = self.put_record(self.device, other, 'r1', 0, name='there')
        self.assertEqual(status, 201, 'the same record id in another vault is a separate record')
        self.assertEqual([row['name'] for row in self.records(self.device, other)['records']], ['there'])
        self.assertEqual([row['name'] for row in self.records(self.device, self.vid)['records']], ['here'])

    def test_record_routes_refuse_a_web_cookie(self):
        vault = self.vault(self.jar, 'Cookie vault')['id']
        self.assertEqual(self.request('GET', '/v1/vaults/%s/records' % vault, cookie=self.jar)[0], 401)
        self.assertEqual(self.request('PUT', '/v1/vaults/%s/records/r1' % vault,
                                      {'base_version': 0, 'operation': 'op-1', 'name': 'n'},
                                      cookie=self.jar)[0], 401)
        self.assertEqual(self.request('DELETE', '/v1/vaults/%s/records/r1' % vault, cookie=self.jar)[0], 401)
        # ... while the session cookie still works on the web routes
        self.assertEqual(self.request('GET', '/v1/vaults', cookie=self.jar)[0], 200)


if __name__ == '__main__':
    unittest.main()
