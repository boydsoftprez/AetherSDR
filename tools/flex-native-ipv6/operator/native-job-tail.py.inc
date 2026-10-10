    # Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT

    import pathlib, time, uuid
    files = FILES_LITERAL
    work = 'var/tmp/nereus-inprocess-' + uuid.uuid4().hex
    os.mkdir(work, 0o700, dir_fd=root_fd)
    report = {'purpose': 'owner-authorized bounded native in-process experiment', 'workdir': '/'+work}
    cid = None
    try:
        for name, source in files.items():
            data = pathlib.Path(source).read_bytes()
            fd = os.open(work+'/'+name, os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_CLOEXEC, 0o700, dir_fd=root_fd)
            try:
                view = memoryview(data)
                while view:
                    count = os.write(fd, view)
                    if count <= 0: raise RuntimeError('incomplete host file write')
                    view = view[count:]
            finally: os.close(fd)
        containers=request('GET','/containers/json?all=true')
        matches=[c for c in containers if any(n.strip('/')=='nereus-net-probe' for n in c.get('Names',[]))]
        if len(matches)!=1:raise RuntimeError('expected diagnostic container')
        existing=request('GET','/containers/'+matches[0]['Id']+'/json')
        code='WORKDIR = '+repr('/'+work)+'\n'+'EXPECTED_SHA256 = '+repr(EXPECTED_SHA_LITERAL)+'\n'+JOB_LITERAL
        created=request('POST','/containers/create?name=nereus-inprocess-job-'+uuid.uuid4().hex[:10], {
            'Image':existing['Image'],'User':'0:0','Entrypoint':['/usr/bin/nsenter'],
            'Cmd':['-t','1','-m','-n','--','/usr/bin/python3','-c',code],
            'HostConfig':{'Privileged':True,'PidMode':'host','NetworkMode':'host','RestartPolicy':{'Name':'no'}},
            'Labels':{'nereus.purpose':'bounded-native-inprocess-job'}})
        cid=created['Id'];report['job_container']=cid
        request('POST','/containers/'+cid+'/start')
        deadline=time.monotonic()+600
        while time.monotonic()<deadline:
            state=request('GET','/containers/'+cid+'/json')['State']
            if not state.get('Running'):break
            time.sleep(.5)
        else:
            request('POST','/containers/'+cid+'/kill?signal=KILL')
            raise RuntimeError('bounded native job timeout')
        report['wait']=request('POST','/containers/'+cid+'/wait?condition=not-running')
        report['logs']=request('GET','/containers/'+cid+'/logs?stdout=true&stderr=true')
    finally:
        if cid is not None:
            request('DELETE','/containers/'+cid+'?force=true')
            report['job_container_deleted']=True
        folder=pathlib.Path('/proc/self/fd/'+str(root_fd))/work
        # Jobs must remove subdirectories and service modifications themselves.
        for path in folder.iterdir():
            if path.is_file():path.unlink()
        os.rmdir(work, dir_fd=root_fd)
        report['temporary_host_directory_deleted']=True
    print(json.dumps(report,indent=2),flush=True)
finally:
    listener.close()
    for fd in received:os.close(fd)
