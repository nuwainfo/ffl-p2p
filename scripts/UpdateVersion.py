#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# FastFileLink CLI - Fast, no-fuss file sharing
# Copyright (C) 2025-2026 FastFileLink contributors

from dataclasses import dataclass
from pathlib import Path

import argparse
import re


PROJECT_ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class VersionLocation:
    relativePath: Path
    pattern: re.Pattern[str]

    @property
    def description(self) -> str:
        return str(self.relativePath)

    def read(self, root: Path) -> tuple[str, str]:
        path = root / self.relativePath
        content = path.read_bytes().decode('utf-8')
        matches = list(self.pattern.finditer(content))
        if len(matches) != 1:
            raise RuntimeError(
                f'expected exactly one version marker in {self.description}'
            )

        return content, matches[0].group('version')

    def updateContent(self, content: str, version: str) -> str:
        match = self.pattern.search(content)
        if match is None:
            raise RuntimeError(f'version marker disappeared from {self.description}')

        start, end = match.span('version')
        return content[:start] + version + content[end:]


@dataclass(frozen=True)
class VersionDocument:
    location: VersionLocation
    content: str
    version: str

    def updatedContent(self, version: str) -> str:
        return self.location.updateContent(self.content, version)


class ProjectVersionManager:
    VERSION_PATTERN = re.compile(
        r'^[0-9]+\.[0-9]+\.[0-9]+(?:[-+._A-Za-z0-9]*)?$'
    )
    PYPROJECT = VersionLocation(
        Path('pyproject.toml'),
        re.compile(
            r'^(?P<prefix>version[ \t]*=[ \t]*")'
            r'(?P<version>[^"]+)'
            r'(?P<suffix>"[ \t]*\r?)$',
            re.MULTILINE,
        ),
    )
    LOCATIONS = (
        PYPROJECT,
        VersionLocation(
            Path('src/ffl_p2p/__init__.py'),
            re.compile(
                r'^(?P<prefix>__version__[ \t]*=[ \t]*")'
                r'(?P<version>[^"]+)'
                r'(?P<suffix>"[ \t]*\r?)$',
                re.MULTILINE,
            ),
        ),
        VersionLocation(
            Path('native/P2P.c'),
            re.compile(
                r'(?P<prefix>'
                r'PyModule_AddStringConstant\('
                r'module,\s*"FFL_P2P_NATIVE_BUILD",\s*"'
                r')'
                r'(?P<version>[^"]+)'
                r'(?P<suffix>"\);)'
            ),
        ),
    )

    @staticmethod
    def _validateVersions(documents: list[VersionDocument]) -> str:
        versions = {document.version for document in documents}
        if len(versions) == 1:
            return documents[0].version

        details = ', '.join(
            f'{document.location.description}={document.version}'
            for document in documents
        )
        raise RuntimeError(f'project version markers are inconsistent: {details}')

    def __init__(self, root: Path = PROJECT_ROOT):
        self.root = root

    @property
    def projectVersion(self) -> str:
        _content, version = self.PYPROJECT.read(self.root)
        return version

    def _readDocuments(self) -> list[VersionDocument]:
        documents = []
        for location in self.LOCATIONS:
            content, version = location.read(self.root)
            documents.append(VersionDocument(location, content, version))

        return documents

    def _writeDocuments(
        self,
        documents: list[VersionDocument],
        version: str,
    ) -> None:
        updates = [
            (
                document,
                document.updatedContent(version).encode('utf-8'),
            )
            for document in documents
        ]
        writtenDocuments = []

        try:
            for document, content in updates:
                path = self.root / document.location.relativePath
                path.write_bytes(content)
                writtenDocuments.append(document)
        except OSError:
            for document in reversed(writtenDocuments):
                path = self.root / document.location.relativePath
                path.write_bytes(document.content.encode('utf-8'))

            raise

    def update(self, version: str) -> None:
        if not self.VERSION_PATTERN.fullmatch(version):
            raise ValueError(f'invalid version: {version}')

        documents = self._readDocuments()
        self._validateVersions(documents)

        self._writeDocuments(documents, version)

        updatedDocuments = self._readDocuments()
        updatedVersion = self._validateVersions(updatedDocuments)
        if updatedVersion != version:
            raise RuntimeError(
                f'project version update verification failed: {updatedVersion}'
            )


def getProjectVersion(root: Path = PROJECT_ROOT) -> str:
    return ProjectVersionManager(root).projectVersion


def updateProjectVersion(version: str, root: Path = PROJECT_ROOT) -> None:
    ProjectVersionManager(root).update(version)


def main() -> int:
    parser = argparse.ArgumentParser(description='Update the ffl-p2p project version')
    parser.add_argument('version')
    args = parser.parse_args()

    updateProjectVersion(args.version)
    print(args.version)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
