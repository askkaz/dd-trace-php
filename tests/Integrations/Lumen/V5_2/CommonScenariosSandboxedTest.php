<?php

namespace DDTrace\Tests\Integrations\Lumen\V5_2;

use DDTrace\Tests\Common\SpanAssertion;
use DDTrace\Tests\Common\WebFrameworkTestCase;
use DDTrace\Tests\Frameworks\Util\Request\RequestSpec;

class CommonScenariosSandboxedTest extends WebFrameworkTestCase
{
    const IS_SANDBOX = true;

    protected static function getAppIndexScript()
    {
        return __DIR__ . '/../../../Frameworks/Lumen/Version_5_2/public/index.php';
    }

    protected static function getEnvs()
    {
        return array_merge(parent::getEnvs(), [
            'DD_SERVICE_NAME' => 'lumen_test_app',
        ]);
    }

    /**
     * @dataProvider provideSpecs
     * @param RequestSpec $spec
     * @param array $spanExpectations
     * @throws \Exception
     */
    public function testScenario(RequestSpec $spec, array $spanExpectations)
    {
        $traces = $this->tracesFromWebRequest(function () use ($spec) {
            $this->call($spec);
        });

        $this->assertFlameGraph($traces, $spanExpectations);
    }

    public function provideSpecs()
    {
        return $this->buildDataProvider(
            [
                'A simple GET request returning a string' => [
                    SpanAssertion::build(
                        'lumen.request',
                        'lumen_test_app',
                        'web',
                        'GET simple_route'
                    )->withExactTags([
                        'lumen.route.name' => 'simple_route',
                        'lumen.route.action' => 'App\Http\Controllers\ExampleController@simple',
                        'http.method' => 'GET',
                        'http.url' => 'http://localhost:9999/simple',
                        'http.status_code' => '200',
                        'integration.name' => 'lumen',
                    ]),
                ],
                'A simple GET request with a view' => [
                    SpanAssertion::build(
                        'lumen.request',
                        'lumen_test_app',
                        'web',
                        'GET App\Http\Controllers\ExampleController@simpleView'
                    )->withExactTags([
                        'lumen.route.action' => 'App\Http\Controllers\ExampleController@simpleView',
                        'http.method' => 'GET',
                        'http.url' => 'http://localhost:9999/simple_view',
                        'http.status_code' => '200',
                        'integration.name' => 'lumen',
                    ])->withChildren([
                        SpanAssertion::build(
                            'laravel.view.render',
                            'lumen_test_app',
                            'web',
                            'simple_view'
                        )->withExactTags([
                            'integration.name' => 'laravel',
                        ])->withChildren([
                            SpanAssertion::build(
                                'lumen.view',
                                'lumen_test_app',
                                'web',
                                '*/resources/views/simple_view.blade.php'
                            )->withExactTags([
                                'integration.name' => 'laravel',
                            ]),
                            SpanAssertion::build(
                                'laravel.event.handle',
                                'lumen_test_app',
                                'web',
                                'composing: simple_view'
                            )->withExactTags([
                                'integration.name' => 'laravel',
                            ]),
                        ]),
                        SpanAssertion::build(
                            'laravel.event.handle',
                            'lumen_test_app',
                            'web',
                            'creating: simple_view'
                        )->withExactTags([
                            'integration.name' => 'laravel',
                        ])
                    ]),
                ],
                'A GET request with an exception' => [
                    SpanAssertion::build(
                        'lumen.request',
                        'lumen_test_app',
                        'web',
                        'GET App\Http\Controllers\ExampleController@error'
                    )->withExactTags([
                        'lumen.route.action' => 'App\Http\Controllers\ExampleController@error',
                        'http.method' => 'GET',
                        'http.url' => 'http://localhost:9999/error',
                        'http.status_code' => '500',
                        'integration.name' => 'lumen',
                    ])->withExistingTagsNames([
                        'error.stack',
                    ])->setError('Exception', 'Controller error'),
                ],
            ]
        );
    }
}