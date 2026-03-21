from setuptools import Extension, setup

module = Extension(
    'immuset',
    sources=['immuset.c', 'hamt_set.c'],
    # No Py_LIMITED_API define
    )

setup(
    name='immuset',
    version='0.1.0',
    description='Immutable set using Hash Array Mapped Trie (HAMT)',
    ext_modules=[module],
    python_requires='>=3.6',
    )
