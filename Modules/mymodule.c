#define PY_SSIZE_T_CLEAN
#include <Python.h>

typedef union _PyStackRef {
    uintptr_t bits;
} _PyStackRef;

// Dummy definition: real definition is in pycore_code.h
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
    PyObject *f_locals_cache;   /* Backwards compatibility for PyEval_GetLocals */
    PyObject *_f_frame_data[1]; /* Frame data if this frame object owns the frame */
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
    if (src->stackpointer == NULL) {
        return;
    }
    _PyStackRef *src_stack_base = _PyFrame_Stackbase(src);
    _PyStackRef *dest_stack_base = _PyFrame_Stackbase(dest);
    Py_ssize_t stack_size = src->stackpointer - src_stack_base;
    memcpy(dest_stack_base, src_stack_base, stack_size * sizeof(_PyStackRef));
    dest->stackpointer = dest_stack_base + stack_size;
}

Py_ssize_t get_instr_offset(struct _frame *frame) {
    PyCodeObject *code = PyFrame_GetCode(frame);
    Py_ssize_t first_instr_addr = (Py_ssize_t) code->co_code_adaptive;
    Py_ssize_t current_instr_addr = (Py_ssize_t) frame->f_frame->instr_ptr;
    return current_instr_addr - first_instr_addr;
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

PyObject *GetFrameLocalsFromFrame(PyObject *frame) {
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

// Allocate something that's not part of Python
_PyInterpreterFrame *AllocateFrameToMigrate(size_t size) {
    return (_PyInterpreterFrame *)malloc(size * sizeof(PyObject*));
}

PyObject *deepcopy_object(PyObject *obj) {
    if (obj == NULL) {
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
    // Strong reference
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

void copy_localsplus(_PyInterpreterFrame *to_copy, _PyInterpreterFrame *new_frame, int nlocals, int deepcopy) {
    if (deepcopy) {
        for (int i = 0; i < nlocals; i++) {
            PyObject *local = (PyObject*) to_copy->localsplus[i].bits;
            PyObject *local_copy = deepcopy_object(local);
            new_frame->localsplus[i].bits = (uintptr_t)local_copy;
        }
    } else {
        memcpy(new_frame->localsplus, to_copy->localsplus, nlocals * sizeof(_PyStackRef));
    }
}

PyFrameObject *create_copied_frame(PyThreadState *tstate, _PyInterpreterFrame *to_copy, PyCodeObject *copy_code_obj, PyObject *LocalCopy, size_t offset, int push_frame, int deepcopy_localsplus, int set_previous, int copy_stack_flag) {
    int nlocals = copy_code_obj->co_nlocalsplus;

    PyFrameObject *new_frame = PyFrame_New(tstate, copy_code_obj, to_copy->f_globals, LocalCopy);

    _PyInterpreterFrame *stack_frame;
    if (push_frame) {
        stack_frame = _PyThreadState_PushFrame(tstate, copy_code_obj->co_framesize);
    } else {
        stack_frame = AllocateFrameToMigrate(copy_code_obj->co_framesize);
    }

    new_frame->f_frame = stack_frame;

    copy_localsplus(to_copy, new_frame->f_frame, nlocals, deepcopy_localsplus);

    new_frame->f_frame->owner = to_copy->owner;
    new_frame->f_frame->previous = set_previous ? to_copy : NULL;
    new_frame->f_frame->f_funcobj = deepcopy_object(to_copy->f_funcobj);
    PyObject *executable_copy = deepcopy_object((PyObject*) to_copy->f_executable.bits);
    new_frame->f_frame->f_executable.bits = (uintptr_t)executable_copy;
    new_frame->f_frame->f_globals = to_copy->f_globals;
    new_frame->f_frame->f_builtins = to_copy->f_builtins;
    new_frame->f_frame->f_locals = to_copy->f_locals;
    new_frame->f_frame->frame_obj = new_frame;
    new_frame->f_frame->stackpointer = new_frame->f_frame->localsplus + nlocals;
    new_frame->f_frame->instr_ptr = copy_code_obj->co_code_adaptive + offset;

    if (copy_stack_flag) {
        copy_stack(to_copy, new_frame->f_frame);
    }

    return new_frame;
}

static PyObject *copy_frame(PyObject *self, PyObject *args) {
    struct _frame *frame = (struct _frame*) PyEval_GetFrame();
    _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *)deepcopy_object((PyObject*)code);

    Py_ssize_t offset = get_instr_offset(frame) + 5 * sizeof(_CodeUnit);
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*)to_copy);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = create_copied_frame(tstate, to_copy, copy_code_obj, LocalCopy, offset, 0, 1, 0, 1);

    PyObject *capsule = frame_copy_capsule_create(new_frame, offset);
    Py_DECREF(copy_code_obj);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return capsule;
}

static PyObject *copy_and_run_frame(PyObject *self, PyObject *args) {
    struct _frame *frame = (struct _frame*) PyEval_GetFrame();
    _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *)deepcopy_object((PyObject*)code);

    Py_ssize_t offset = get_instr_offset(frame) + 5 * sizeof(_CodeUnit);
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*)to_copy);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = create_copied_frame(tstate, to_copy, copy_code_obj, LocalCopy, offset, 1, 0, 1, 1);

    PyObject *res = PyEval_EvalFrame(new_frame);
    Py_DECREF(copy_code_obj);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return res;
}

static PyObject *_copy_run_frame_from_capsule(PyObject *capsule) {
    if (PyErr_Occurred()) {
        PyErr_Print();
        return NULL;
    }

    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)PyCapsule_GetPointer(capsule, copy_frame_capsule_name);
    if (copy_capsule == NULL) {
        return NULL;
    }

    PyFrameObject *frame = copy_capsule->frame;
    size_t offset = copy_capsule->offset;
    _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *)deepcopy_object((PyObject*)code);

    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*)to_copy);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = create_copied_frame(tstate, to_copy, copy_code_obj, LocalCopy, offset, 1, 0, 1, 0);

    PyObject *res = PyEval_EvalFrame(new_frame);
    Py_DECREF(copy_code_obj);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return res;
}

static PyObject *run_frame(PyObject *self, PyObject *args) {
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return NULL;
    }
    return _copy_run_frame_from_capsule(capsule);
}

static PyMethodDef MyMethods[] = {
    {"copy_and_run_frame", copy_and_run_frame, METH_VARARGS, "Copy the current frame and run it"},
    {"copy_frame", copy_frame, METH_VARARGS, "Copy the current frame"},
    {"run_frame", run_frame, METH_VARARGS, "Run the frame from the capsule"},
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
