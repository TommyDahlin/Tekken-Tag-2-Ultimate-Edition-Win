import collections
import copy
import types
import unittest

from ttt2char.convert import native_moves as N, tagsystem as T


class NativeMoves(unittest.TestCase):
    def converter(self):
        end = dict(direction=0x8000, button=0, req=0, extradata=0,
                   win_start=39, win_end=32767, start_frame=39, move_id=0x8001, option=0)
        capture = dict(end, direction=0, win_start=12, win_end=14, start_frame=12, move_id=2)
        fields = ('default','crouch','left','crouch_left','right','crouch_right','back','crouch_back',
                  'pb_front','pb_back','pb_left','pb_right','dir_front','dir_back','dir_left','dir_right',
                  'vertical_pushback','ch','crouch_ch','pb_front_ch','dir_front_ch','block','downed')
        reactions = [{f:i for i,f in enumerate(fields)}, {f:100+i for i,f in enumerate(fields)}]
        tables = dict(moves=[dict(cancel=0, hit_condition=0),dict(cancel=2,hit_condition=0),
                             dict(cancel=0,hit_condition=0)],
                      cancels=[capture,end,capture.copy(),end.copy()], group_cancels=[],
                      requirements=[dict(condition=690,param=0)], cancel_extras=[dict(value=26)],
                      reactions=reactions,hit_conditions=[dict(req=0,damage=27,reactions=0)])
        return types.SimpleNamespace(T=tables,dn=dict(zip(N.THROW_ATTEMPTS,(0,1))),stats=collections.Counter())

    def test_wall_capture_copies_guards_and_preserves_normal_routes(self):
        c=self.converter();before=copy.deepcopy(c.T)
        template=({k:v for k,v in before['cancels'][0].items() if k not in ('req','extradata','move_id')},
                  [(27,2536),(184,80),(159,8233)],6876)
        N.add_wall_throw(c,template,2)
        for i in (0,1):
            rows=T.cancels(c.T,c.T['moves'][i]['cancel'])
            self.assertEqual(rows[0],before['cancels'][2*i])
            self.assertEqual(rows[-1],before['cancels'][2*i+1])
            wall=rows[-2]
            self.assertEqual(T.reqs(c.T,wall['req']),template[1])
            self.assertEqual(c.T['cancel_extras'][wall['extradata']]['value'],6876)
            self.assertEqual((wall['win_start'],wall['win_end'],wall['start_frame']),(12,14,12))
        self.assertEqual(c.T['moves'][2]['cancel'],c.T['moves'][0]['cancel'])
        self.assertEqual(c.T['cancels'][:4],before['cancels'])

    def test_forward_wall_capture_uses_its_own_later_window(self):
        c=self.converter()
        c.dn={name+'F':i for name,i in c.dn.items()}
        row={k:v for k,v in c.T['cancels'][0].items() if k not in ('req','extradata','move_id')}
        row.update(win_start=15,win_end=17,start_frame=15)
        N.add_wall_throw(c,(row,[(27,2536),(184,80),(159,8233)],6876),2,forward=True)
        for move in c.T['moves'][:2]:
            wall=T.cancels(c.T,move['cancel'])[-2]
            self.assertEqual((wall['win_start'],wall['win_end'],wall['start_frame']),(15,17,15))

    def test_air_normal_keeps_grounded_damage_and_counter_hit_slots(self):
        c=self.converter();before=copy.deepcopy(c.T)
        N.air_normal_reaction(c,0,1)
        hits=T.hits(c.T,c.T['moves'][0]['hit_condition'])
        self.assertEqual(hits[1:],before['hit_conditions'])
        self.assertEqual(hits[0]['damage'],27)
        self.assertEqual(T.reqs(c.T,hits[0]['req']),[(466,0)])
        reaction=c.T['reactions'][hits[0]['reactions']]
        for field in ('ch','crouch_ch','pb_front_ch','dir_front_ch','block','downed'):
            self.assertEqual(reaction[field],before['reactions'][0][field])
        self.assertEqual(reaction['default'],before['reactions'][1]['default'])
        self.assertEqual(c.T['hit_conditions'][0],before['hit_conditions'][0])
        self.assertEqual(c.T['moves'][1]['hit_condition'],0)

    def test_copy_air_reaction_preserves_string_damage_and_grounded_hit(self):
        c=self.converter()
        c.T['requirements'] += [dict(condition=466,param=0),dict(condition=690,param=0)]
        c.T['hit_conditions'] += [dict(req=1,damage=20,reactions=1),dict(req=0,damage=20,reactions=0)]
        c.T['moves'][1]['hit_condition']=1
        before=copy.deepcopy(c.T)
        N.copy_air_reaction(c,0,1)
        rows=T.hits(c.T,c.T['moves'][0]['hit_condition'])
        self.assertEqual(rows[0],dict(req=1,damage=27,reactions=1))
        self.assertEqual(rows[1:],[before['hit_conditions'][0]])
        self.assertEqual(c.T['moves'][1],before['moves'][1])
        self.assertEqual(c.T['hit_conditions'][:3],before['hit_conditions'])
        N.copy_air_reaction(c,0,1)
        self.assertEqual(len(T.hits(c.T,c.T['moves'][0]['hit_condition'])),2)

    def test_breath_visual_keeps_damage_and_adds_no_projectile(self):
        c=self.converter()
        c.T['extra_move_properties']=[dict(frame=20,id=0x8058,value=15),dict(frame=0,id=0,value=0)]
        for move in c.T['moves']:move['extraprops']=0
        before=copy.deepcopy(c.T)
        N.add_breath_visual(c,0,(115,127,142),1,5)
        props=T.props(c.T,c.T['moves'][0]['extraprops'])
        self.assertEqual(props[0],before['extra_move_properties'][0])
        self.assertEqual(props[1:],[dict(frame=f,id=0x801f,value=0x501) for f in (115,127,142)])
        self.assertEqual(c.T['hit_conditions'],before['hit_conditions'])
        self.assertNotIn('projectiles',c.T)
        self.assertEqual(c.T['moves'][1],before['moves'][1])

    def test_existing_air_branch_is_rejected(self):
        c=self.converter()
        c.T['requirements']=[dict(condition=466,param=0),dict(condition=690,param=0)]
        with self.assertRaisesRegex(ValueError,'already has an airborne'):
            N.air_normal_reaction(c,0,1)


class NativeInputRedirect(unittest.TestCase):
    def test_group_offsets_and_terminators_are_not_move_ids(self):
        rows=[dict(direction=d,move_id=1621) for d in (0,0x8001,0x800b,0x8000)]
        groups=[dict(direction=d,move_id=1621) for d in (0x70,0x800c)]
        c=types.SimpleNamespace(T=dict(cancels=rows,group_cancels=groups))
        self.assertEqual(N.redirect_move_inputs(c,1621,2425),3)
        self.assertEqual([r['move_id'] for r in rows],[2425,2425,1621,1621])
        self.assertEqual([r['move_id'] for r in groups],[2425,1621])


class NativeGroupValidation(unittest.TestCase):
    def test_crashing_move_index_in_group_offset_is_rejected(self):
        c=types.SimpleNamespace(T=dict(cancels=[dict(direction=0x800b,move_id=2425)],
                                      group_cancels=[dict(direction=0x800c)]*1665))
        with self.assertRaisesRegex(ValueError,'outside group_cancels'):
            N.validate_command_groups(c)
        c.T['cancels'][0]['move_id']=1621
        N.validate_command_groups(c)


if __name__=='__main__':unittest.main()
