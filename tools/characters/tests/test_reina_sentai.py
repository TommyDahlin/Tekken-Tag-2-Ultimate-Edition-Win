import collections,copy,types,unittest
from ttt2char.convert import move_overrides as O, tagsystem as T

class ReinaSentai(unittest.TestCase):
    def converter(self,mode=0x185,end=24):
        row=dict(direction=0,button=0x4000000c,req=0,extradata=0,win_start=1,win_end=end,start_frame=20,move_id=2,option=0)
        stop=dict(row,direction=0x8000,button=0,move_id=0)
        tables=dict(moves=[{'cancel':0},{'cancel':0},{'cancel':2}],cancels=[row,stop,stop],
                    cancel_extras=[{'value':mode}],requirements=[{'condition':481,'param':0},{'condition':690,'param':0}])
        return types.SimpleNamespace(T=tables,kn={'SEN34':2},newidx={2:2},stats=collections.Counter())
    def test_restores_both_shared_stance_routes_without_changing_guard_or_timing(self):
        c=self.converter();before=copy.deepcopy(c.T['cancels'][0]);O.normalize_stance_input(c,'SEN34',0x185,0x182)
        self.assertEqual(c.T['cancels'][0],before)
        for move in c.T['moves'][:2]:
            row=T.cancels(c.T,move['cancel'])[0]
            self.assertEqual({k:v for k,v in row.items() if k!='extradata'},{k:v for k,v in before.items() if k!='extradata'})
            self.assertEqual(c.T['cancel_extras'][row['extradata']]['value'],0x182)
        self.assertEqual(c.stats['native stance inputs restored'],2)
    def test_unbounded_or_unknown_input_is_not_enabled(self):
        for mode,end in [(0x2800,24),(0x185,32767)]:
            c=self.converter(mode,end);before=copy.deepcopy(c.T)
            with self.assertRaises(ValueError):O.normalize_stance_input(c,'SEN34',0x185,0x182)
            self.assertEqual(c.T,before)
    def test_repeated_application_is_idempotent(self):
        c=self.converter();O.normalize_stance_input(c,'SEN34',0x185,0x182);before=copy.deepcopy(c.T)
        O.normalize_stance_input(c,'SEN34',0x185,0x182);self.assertEqual(c.T,before)
    def test_definition_preserves_requested_non_bound_finisher(self):
        from ttt2char.characters import reina
        self.assertIs(reina.MOVESET['bounds']['Zbr_sen_WK'],False)
        self.assertIs(reina.MOVESET['bounds']['Zbr_sen_WK2'],False)
        self.assertEqual(reina.MOVESET['native_stance_inputs']['Zbr_sen_WK'],(0x185,0x182))

if __name__=='__main__':unittest.main()
