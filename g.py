import mymodule
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
        mymodule.copy_and_run_frame()
        print('returning from outer')
        x = 45
        return
    else:
        print(f'inner, a={a}, b={b}, c={c}')

fun1(3)

