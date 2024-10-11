import mymodule
import sys
calls = 0

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
        print(frm_copy)
        return_val = mymodule.run_frame(frm_copy)
        print('return_val = ', return_val)
        x = 45
        return
    else:
        print(f'inner, a={a}, b={b}, c={c}')
        return 500

fun1(3)

