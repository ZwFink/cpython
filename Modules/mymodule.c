#define PY_SSIZE_T_CLEAN
#include <Python.h>

typedef union _PyStackRef {
    uintptr_t bits;
} _PyStackRef;

// dummy definition: real definition is in
// pycore_code.h
typedef struct _CodeUnit {
    uint8_t opcode;
    uint8_t oparg;
} _CodeUnit;

struct _frame {
    PyObject_HEAD
    PyFrameObject *f_back;      /* previous frame, or NULL */
    struct _PyInterpreterFrame *f_frame; /* points to the frame data */
    PyObject *f_trace;          /* Trace function */
    int f_lineno;               /* Current line number. Only valid if non-zero */
    char f_trace_lines;         /* Emit per-line trace events? */
    char f_trace_opcodes;       /* Emit per-opcode trace events? */
    PyObject *f_extra_locals;   /* Dict for locals set by users using f_locals, could be NULL */
    /* This is purely for backwards compatibility for PyEval_GetLocals.
       PyEval_GetLocals requires a borrowed reference so the actual reference
       is stored here */
    PyObject *f_locals_cache;
    /* The frame data, if this frame object owns the frame */
    PyObject *_f_frame_data[1];
};

struct _PyInterpreterFrame *
_PyThreadState_PushFrame(PyThreadState *tstate, size_t size);

typedef struct _PyInterpreterFrame {
    _PyStackRef f_executable; /* Deferred or strong reference (code object or None) */
    struct _PyInterpreterFrame *previous;
    PyObject *f_funcobj; /* Strong reference. Only valid if not on C stack */
    PyObject *f_globals; /* Borrowed reference. Only valid if not on C stack */
    PyObject *f_builtins; /* Borrowed reference. Only valid if not on C stack */
    PyObject *f_locals; /* Strong reference, may be NULL. Only valid if not on C stack */
    PyFrameObject *frame_obj; /* Strong reference, may be NULL. Only valid if not on C stack */
    _CodeUnit *instr_ptr; /* Instruction currently executing (or about to begin) */
    _PyStackRef *stackpointer;
    uint16_t return_offset;  /* Only relevant during a function call */
    char owner;
    /* Locals and stack */
    _PyStackRef localsplus[1];
} _PyInterpreterFrame;


static inline _PyStackRef *_PyFrame_Stackbase(_PyInterpreterFrame *f) {
    return f->localsplus + ((PyCodeObject*)f->f_executable.bits)->co_nlocalsplus;
}

void copy_stack(_PyInterpreterFrame *src, _PyInterpreterFrame *dest) {
    _PyStackRef *src_stack_base = _PyFrame_Stackbase(src);
    _PyStackRef *dest_stack_base = _PyFrame_Stackbase(dest);
    if(NULL == src->stackpointer){
        return;
    }
    for (_PyStackRef *ptr = src_stack_base; ptr < src->stackpointer; ptr++) {
        *dest_stack_base = *ptr;
        dest_stack_base++;
    }

    dest_stack_base = _PyFrame_Stackbase(dest);
    Py_ssize_t src_offset = src->stackpointer - src_stack_base;
    dest->stackpointer = dest_stack_base + src_offset;
}


Py_ssize_t get_instr_offset(struct _frame *frame) {
    PyCodeObject *code = PyFrame_GetCode(frame);
    Py_ssize_t first_instr_addr = (Py_ssize_t) code->co_code_adaptive;
    Py_ssize_t current_instr_addr = (Py_ssize_t) frame->f_frame->instr_ptr;
    Py_ssize_t difference = current_instr_addr - first_instr_addr;
    return difference;
}

void print_object(PyObject *obj) {
    if (obj == NULL) {
        printf("Error: NULL object passed\n");
        return;
    }

    PyObject *str = PyObject_Repr(obj);
    if (str == NULL) {
        PyErr_Print();
        return;
    }

    const char *c_str = PyUnicode_AsUTF8(str);
    if (c_str == NULL) {
        Py_DECREF(str);
        PyErr_Print();
        return;
    }

    printf("Object contents: %s\n", c_str);

    Py_DECREF(str);
}

void print_object_type_name(PyObject *obj) {
    if (obj == NULL) {
        printf("Error: NULL object\n");
        return;
    }

    PyObject *type = PyObject_Type(obj);
    if (type == NULL) {
        printf("Error: Could not get object type\n");
        PyErr_Print();
        return;
    }

    PyObject *type_name = PyObject_GetAttrString(type, "__name__");
    if (type_name == NULL) {
        printf("Error: Could not get type name\n");
        PyErr_Print();
        Py_DECREF(type);
        return;
    }

    const char *name = PyUnicode_AsUTF8(type_name);
    if (name == NULL) {
        printf("Error: Could not convert type name to string\n");
        PyErr_Print();
    } else {
        printf("Object type: %s\n", name);
    }

    Py_DECREF(type_name);
    Py_DECREF(type);
}

PyAPI_FUNC(PyFrameObject *) PyFrame_New(PyThreadState *, PyCodeObject *,
                                        PyObject *, PyObject *);
int PyFrame_FastToLocalsWithError(PyObject*);
PyObject *_PyFrame_GetLocals(_PyInterpreterFrame *);

PyObject *
GetFrameLocalsFromFrame(PyObject *frame)
{
     _PyInterpreterFrame *current_frame = (_PyInterpreterFrame *)frame;

    PyObject *locals = _PyFrame_GetLocals(current_frame);
    if (locals == NULL) {
        return NULL;
    }

    if (PyFrameLocalsProxy_Check(locals)) {
        PyObject* ret = PyDict_New();
        if (ret == NULL) {
            Py_DECREF(locals);
            return NULL;
        }
        if (PyDict_Update(ret, locals) < 0) {
            Py_DECREF(ret);
            Py_DECREF(locals);
            return NULL;
        }
        Py_DECREF(locals);
        return ret;
    }

    assert(PyMapping_Check(locals));
    return locals;
}

// allocate something that's not part of Python
_PyInterpreterFrame *AllocateFrameToMigrate(size_t size) {
    return (_PyInterpreterFrame *)malloc(size*sizeof(PyObject*));
}

PyObject *deepcopy_object(PyObject *obj) {
    if(NULL == obj) {
        return NULL;
    }
    PyObject *copy = PyImport_ImportModule("copy");
    PyObject *deepcopy = PyObject_GetAttrString(copy, "deepcopy");
    PyObject *copy_obj = PyObject_CallFunction(deepcopy, "O", obj);
    Py_DECREF(copy);
    Py_DECREF(deepcopy);
    return copy_obj;
}

struct frame_copy_capsule {
    // strong ref
    PyFrameObject *frame;
    size_t offset;
};

static char copy_frame_capsule_name[] = "Frame Capsule Object";

void frame_copy_capsule_destroy(PyObject *capsule) {
    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)PyCapsule_GetPointer(capsule, copy_frame_capsule_name);
    Py_DECREF(copy_capsule->frame);
    free(copy_capsule);
}

PyObject *frame_copy_capsule_create(PyFrameObject *frame, size_t offset) {
    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)malloc(sizeof(struct frame_copy_capsule));
    copy_capsule->frame = frame;
    copy_capsule->offset = offset;
    return PyCapsule_New(copy_capsule, copy_frame_capsule_name, frame_copy_capsule_destroy);
}

static PyObject *copy_frame(PyObject *self, PyObject *args) {
    struct _frame *frame = (struct frame*) PyEval_GetFrame();
    struct _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *) deepcopy_object(code);

    int nlocals = copy_code_obj->co_nlocalsplus;
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*) to_copy);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = PyFrame_New(tstate, copy_code_obj,
    to_copy->f_globals, LocalCopy);
    _PyInterpreterFrame *stack_frame = AllocateFrameToMigrate(copy_code_obj->co_framesize);
    new_frame->f_frame = stack_frame;
    for(int i = 0; i < nlocals; i++) {
        PyObject *local = (PyObject*) to_copy->localsplus[i].bits;
        PyObject *local_copy = deepcopy_object(local);
        new_frame->f_frame->localsplus[i].bits = (uintptr_t)local_copy;
    }
    new_frame->f_frame->owner = to_copy->owner;
    new_frame->f_frame->previous = NULL;
    new_frame->f_frame->f_funcobj = deepcopy_object(to_copy->f_funcobj);
    PyObject *executable_copy = deepcopy_object((PyObject*) to_copy->f_executable.bits);
    new_frame->f_frame->f_executable.bits = (uintptr_t)executable_copy;
    new_frame->f_frame->f_globals = to_copy->f_globals;
    new_frame->f_frame->f_builtins = to_copy->f_builtins;
    new_frame->f_frame->f_locals = to_copy->f_locals;
    new_frame->f_frame->frame_obj = new_frame;
    new_frame->f_frame->stackpointer = new_frame->f_frame->localsplus + nlocals;
    Py_ssize_t offset = get_instr_offset(frame);
    // the instruction trace for calls look slike this:
    // CALL
    // CACHE
    // CACHE
    // CACHE
    // POP_TOP
    // *target instruction here*
    // 'offset' originally points to the CALL instruction
    // we need to skip all the way to the target instruction
    offset += 5 * sizeof(_CodeUnit);
    //PySys_WriteStdout("Offset: %d\n", offset);
    new_frame->f_frame->instr_ptr = copy_code_obj->co_code_adaptive + offset;
    //PySys_WriteStdout("Opcode is: %d\n", new_frame->f_frame->instr_ptr->opcode);
    copy_stack(to_copy, new_frame->f_frame);

    PyObject *capsule = frame_copy_capsule_create(new_frame, offset);
    return capsule;
}


static PyObject *copy_and_run_frame(PyObject *self, PyObject *args) {
    struct _frame *frame = (struct frame*) PyEval_GetFrame();
    struct _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *) deepcopy_object(code);

    int nlocals = copy_code_obj->co_nlocalsplus;
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*) to_copy);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = PyFrame_New(tstate, copy_code_obj,
    to_copy->f_globals, LocalCopy);
    _PyInterpreterFrame *stack_frame = _PyThreadState_PushFrame(tstate, copy_code_obj->co_framesize);
    new_frame->f_frame = stack_frame;
    for(int i = 0; i < nlocals; i++) {
        new_frame->f_frame->localsplus[i] = to_copy->localsplus[i];
    }
    new_frame->f_frame->owner = to_copy->owner;
    new_frame->f_frame->previous = to_copy;
    new_frame->f_frame->f_funcobj = deepcopy_object(to_copy->f_funcobj);
    PyObject *executable_copy = deepcopy_object((PyObject*) to_copy->f_executable.bits);
    new_frame->f_frame->f_executable.bits = (uintptr_t)executable_copy;
    new_frame->f_frame->f_globals = to_copy->f_globals;
    new_frame->f_frame->f_builtins = to_copy->f_builtins;
    new_frame->f_frame->f_locals = to_copy->f_locals;
    new_frame->f_frame->frame_obj = new_frame;
    new_frame->f_frame->stackpointer = new_frame->f_frame->localsplus + nlocals;
    Py_ssize_t offset = get_instr_offset(frame);
    // the instruction trace for calls look slike this:
    // CALL
    // CACHE
    // CACHE
    // CACHE
    // POP_TOP
    // *target instruction here*
    // 'offset' originally points to the CALL instruction
    // we need to skip all the way to the target instruction
    offset += 5 * sizeof(_CodeUnit);
    //PySys_WriteStdout("Offset: %d\n", offset);
    new_frame->f_frame->instr_ptr = copy_code_obj->co_code_adaptive + offset;
    //PySys_WriteStdout("Opcode is: %d\n", new_frame->f_frame->instr_ptr->opcode);
    copy_stack(to_copy, new_frame->f_frame);


    PyObject *res = PyEval_EvalFrame(new_frame);
    Py_DECREF(code);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return res;
}

static PyObject *_copy_run_frame_from_capsule(PyObject *capsule) {
    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)PyCapsule_GetPointer(capsule, copy_frame_capsule_name);
    // check if error was set
    if(PyErr_Occurred()) {
        // get the error info
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        // print the error
        PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);
        PyObject *str_exc = PyObject_Str(pvalue);
        PySys_WriteStdout("Error occurred: %s\n", PyUnicode_AsUTF8(str_exc));

        PySys_WriteStdout("Error occurred\n");
        return NULL;
    }

    PyFrameObject *frame = copy_capsule->frame;
    size_t offset = copy_capsule->offset;
    struct _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *) deepcopy_object(code);

    int nlocals = copy_code_obj->co_nlocalsplus;
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*) to_copy);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = PyFrame_New(tstate, copy_code_obj,
    to_copy->f_globals, LocalCopy);
    _PyInterpreterFrame *stack_frame = _PyThreadState_PushFrame(tstate, copy_code_obj->co_framesize);
    new_frame->f_frame = stack_frame;
    for(int i = 0; i < nlocals; i++) {
        new_frame->f_frame->localsplus[i] = to_copy->localsplus[i];
    }
    new_frame->f_frame->owner = to_copy->owner;
    new_frame->f_frame->previous = to_copy;
    new_frame->f_frame->f_funcobj = deepcopy_object(to_copy->f_funcobj);
    PyObject *executable_copy = deepcopy_object((PyObject*) to_copy->f_executable.bits);
    new_frame->f_frame->f_executable.bits = (uintptr_t)executable_copy;
    new_frame->f_frame->f_globals = to_copy->f_globals;
    new_frame->f_frame->f_builtins = to_copy->f_builtins;
    new_frame->f_frame->f_locals = to_copy->f_locals;
    new_frame->f_frame->frame_obj = new_frame;
    new_frame->f_frame->stackpointer = new_frame->f_frame->localsplus + nlocals;
    new_frame->f_frame->instr_ptr = copy_code_obj->co_code_adaptive + offset;

    PyObject *res = PyEval_EvalFrame(new_frame);
    Py_DECREF(code);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return res;
}

static PyObject *run_frame(PyObject *self, PyObject *args) {
    // get teh capsule from args
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return NULL;
    }
    PyObject *res = _copy_run_frame_from_capsule(capsule);
    return res;
}


static PyMethodDef MyMethods[] = {
    {"copy_and_run_frame", copy_and_run_frame, METH_VARARGS, "Copy the current frame and run it"},
    {"copy_frame", copy_frame, METH_VARARGS, "Copy the current frame"},
    {"run_frame", run_frame, METH_VARARGS, "run the current frame"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef mymodule = {
    PyModuleDef_HEAD_INIT,
    "mymodule",
    "A module that defines the 'abcd' function",
    -1,
    MyMethods
};

PyMODINIT_FUNC PyInit_mymodule(void) {
    return PyModule_Create(&mymodule);
}