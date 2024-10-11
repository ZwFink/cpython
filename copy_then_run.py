import mymodule
import sys
calls = 0

def do_run_frame(frm):
    return_val = mymodule.run_frame(frm)
    print('return_val = ', return_val)

def fun1(c):
    global calls
    print("Running with calls == ", calls)
    if calls == 0:
        calls += 1
        a = 5
        b = 45
        c = c + 10
        print('Calling inner')
        frm_copy = mymodule.copy_frame()
        print('returning from outer')
        # return_val = mymodule.run_frame(frm_copy)
        # print('return_val = ', return_val)
        x = 45
        print(x)
        if calls == 1:
            calls += 1
            hidden_inner = 55
            return frm_copy
        x += 1
        print(f'inner, a={a}, b={b}, c={c}')
        return x

frm = fun1(13)
do_run_frame(frm)
do_run_frame(frm)
do_run_frame(frm)
do_run_frame(frm)


