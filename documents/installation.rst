Installation
=================

Prerequisites
-------------

RaNNC works only with CUDA devices (CPU only or TPU environments are not supported).
RaNNC requires the following libraries and tools at runtime.

* *CUDA*: A CUDA runtime must be available in the runtime environment. Currently, RaNNC has been tested with CUDA 10.2 and 11.0.
* *NCCL*: NCCL (Version >= 2.8.4 is required) must be available in the runtime environment. RaNNC uses NCCL both for allreduce and P2P communications.

  * We recommend *NOT* to use v2.10.3 because RaNNC may encounter a `bug <https://github.com/NVIDIA/nccl/issues/560>`_.
* *MPI*: A program using RaNNC must be launched with MPI. MPI libraries must also be available at runtime. RaNNC has been tested with OpenMPI v4.0.5.
* *libstd++*: ``libstd++`` must support ``GLIBCXX_3.4.21`` to use the distributed ``pip`` packages (these packages are built with gcc 5.4.0).


Installation
------------

The current version (``0.7.3``) of RaNNC requires PyTorch v1.10.1.
``pip`` packages for ``linux_x86_64`` are available for the following combinations of Python and CUDA versions.

* Python version: 3.7, 3.8, 3.9
* CUDA version: 10.2, 11.3

The followings show command to create a new conda environment and install RaNNC.
(The package version should be specified as ``0.7.3+cu[CUDA_VERSION_WITHOUT_DOT]``)

.. code-block:: bash

  conda create -n rannc python=3.8
  conda activate rannc
  conda install pytorch==1.10.1 cudatoolkit=10.2 -c pytorch
  pip install pyrannc==0.7.3+cu102 -f https://nict-wisdom.github.io/rannc/installation.html


Use the following links to manually download the packages.

* :download:`For Python 3.7 / CUDA 10.2 <https://github.com/nict-wisdom/rannc/releases/download/v0.7.3/pyrannc-0.7.3+cu102-cp37-cp37m-linux_x86_64.whl>`
* :download:`For Python 3.8 / CUDA 10.2 <https://github.com/nict-wisdom/rannc/releases/download/v0.7.3/pyrannc-0.7.3+cu102-cp38-cp38-linux_x86_64.whl>`
* :download:`For Python 3.9 / CUDA 10.2 <https://github.com/nict-wisdom/rannc/releases/download/v0.7.3/pyrannc-0.7.3+cu102-cp39-cp39-linux_x86_64.whl>`
* :download:`For Python 3.7 / CUDA 11.3 <https://github.com/nict-wisdom/rannc/releases/download/v0.7.3/pyrannc-0.7.3+cu113-cp37-cp37m-linux_x86_64.whl>`
* :download:`For Python 3.8 / CUDA 11.3 <https://github.com/nict-wisdom/rannc/releases/download/v0.7.3/pyrannc-0.7.3+cu113-cp38-cp38-linux_x86_64.whl>`
* :download:`For Python 3.9 / CUDA 11.3 <https://github.com/nict-wisdom/rannc/releases/download/v0.7.3/pyrannc-0.7.3+cu113-cp39-cp39-linux_x86_64.whl>`

If the above packages do not match your Python/CUDA versions, create a suitable package using ``Makefile``
in ``docker/``. ``make.sh`` shows the commands to create wheel packages.
